#include "map_view.h"
#include "map_geo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "map_platform.h"

static const char *TAG = "map_view";

#define REFRESH_PERIOD_MS       30
#define FOLLOW_EASE             0.28    /* fraction of the remaining gap per tick */
#define FOLLOW_SNAP_PX          0.75    /* below this, jump and stop easing */
#define MOMENTUM_DECAY          0.90
#define MOMENTUM_STOP_PX        0.5
#define DRAG_START_PX           4       /* movement before follow mode is dropped */
#define ROUTE_MIN_STEP_PX       3       /* screen-space decimation of the polyline */
#define ROUTE_MAX_SEGMENTS      600     /* frame-budget guard */
#define TILE_PX                 MAP_TILE_CACHE_TILE_SIZE
#define MV_EDGE_ARROW_INSET     26      /* keeps the arrow off a round bezel */
#define MV_LINE_CLIP_MARGIN     8       /* extra room so round caps are not clipped visibly */
#define ROTATE_EASE             0.18    /* fraction of the remaining turn applied per timer tick */
#define ROTATE_SNAP_DEG         0.15    /* closer than this: just land on the target */
#define TILE_CIRCUMRADIUS       182.0   /* half the diagonal of a 256 px tile, rounded up */

#if !LV_DRAW_COMPLEX
#warning "map_tiles_lvgl8: LV_DRAW_COMPLEX is 0, so image rotation is unavailable and the map will stay north-up on screen"
#endif

typedef struct {
    lv_obj_t               *obj;
    map_tile_cache_handle_t cache;
    map_route_handle_t      route;

    int    zoom;
    int    min_zoom;
    int    max_zoom;
    double center_wx;       /* world pixels at `zoom` */
    double center_wy;

    bool   pos_valid;
    double pos_lat;
    double pos_lon;
    double pos_heading;
    bool   off_route;
    bool   return_active;
    double return_lat;
    double return_lon;
    uint32_t traveled_m;

    bool    follow;
    int32_t follow_offset;

    /* Rotation is stored as the compass bearing that points UP on screen:
     * 0 = north up (default), 90 = east up, 180 = south up. Heading-up is just
     * "bearing up = vehicle heading". */
    double  rot_deg;        /* what is drawn now, [0, 360) */
    double  rot_target;     /* where the timer is easing to */
    bool    heading_up;
    bool    smooth_rot;     /* bilinear tiles while rotated (slower, softer) */

    bool    pressing;
    bool    drag_active;
    int32_t press_travel;
    double  vel_x;
    double  vel_y;

    map_view_style_t style;
    bool             debug;
    bool             warned_segment_cap;

    lv_timer_t *timer;
    uint32_t    cache_version;
} map_view_t;

/* --------------------------------------------------------------- helpers */

static map_view_t *mv_of(lv_obj_t *obj)
{
    return obj ? (map_view_t *)lv_obj_get_user_data(obj) : NULL;
}

static void mv_default_style(map_view_style_t *s)
{
    s->route_color        = lv_color_hex(0x2F7BEF);
    s->route_done_color   = lv_color_hex(0x9AA3AE);
    s->route_casing_color = lv_color_hex(0x123A6E);
    s->marker_color       = lv_color_hex(0x1A73E8);
    s->marker_off_color   = lv_color_hex(0xE03B24);
    s->return_color       = lv_color_hex(0xFF8A3D);
    s->destination_color  = lv_color_hex(0x1DB954);
    s->route_width        = 9;
    s->casing_width       = 15;
    s->marker_radius      = 13;
}

/** Recompute the world-pixel centre so the same place stays centred at a new zoom. */
static void mv_reproject_center(map_view_t *mv, int new_zoom)
{
    double lat = map_geo_world_py_to_lat(mv->center_wy, mv->zoom);
    double lon = map_geo_world_px_to_lon(mv->center_wx, mv->zoom);
    mv->zoom = new_zoom;
    mv->center_wx = map_geo_lon_to_world_px(lon, new_zoom);
    mv->center_wy = map_geo_lat_to_world_py(lat, new_zoom);
}

/** Clamp vertically to the world and wrap horizontally. */
static void mv_clamp_center(map_view_t *mv)
{
    double world = map_geo_world_size_px(mv->zoom);
    if (mv->center_wx < 0)      mv->center_wx += world;
    if (mv->center_wx >= world) mv->center_wx -= world;
    if (mv->center_wy < 0)      mv->center_wy = 0;
    if (mv->center_wy > world)  mv->center_wy = world;
}

static double mv_norm_deg(double d)
{
    d = fmod(d, 360.0);
    return d < 0 ? d + 360.0 : d;
}

/** Signed shortest turn from @p from to @p to, in (-180, 180]. */
static double mv_turn_deg(double to, double from)
{
    double d = mv_norm_deg(to - from);
    return d > 180.0 ? d - 360.0 : d;
}

/** The rotation actually rendered: quantised to LVGL's 0.1 degree so tiles and vectors agree exactly. */
static int mv_angle01(const map_view_t *mv)
{
    return ((int)lround(mv_norm_deg(mv->rot_deg) * 10.0)) % 3600;
}

static void mv_rot_trig(const map_view_t *mv, double *c, double *s)
{
    double th = mv_angle01(mv) / 10.0 * M_PI / 180.0;
    *c = cos(th);
    *s = sin(th);
}

/** Where the follow target wants the centre, given the on-screen marker offset
 *  (which is "down" on screen, whatever the rotation). */
static bool mv_follow_target(map_view_t *mv, double *wx, double *wy)
{
    if (!mv->pos_valid) return false;
    double c, s;
    mv_rot_trig(mv, &c, &s);
    *wx = map_geo_lon_to_world_px(mv->pos_lon, mv->zoom) + (double)mv->follow_offset * s;
    *wy = map_geo_lat_to_world_py(mv->pos_lat, mv->zoom) - (double)mv->follow_offset * c;
    return true;
}

static void mv_content_area(map_view_t *mv, lv_area_t *out)
{
    lv_obj_get_content_coords(mv->obj, out);
}

/* ------------------------------------------------------------- rendering */

/*
 * LVGL 8 differences that matter here:
 *  - drawing goes through an lv_draw_ctx_t, not an lv_layer_t;
 *  - lv_coord_t is int16_t by default, so a point 40 000 px off screen would wrap
 *    and draw a line across the display. Anything that can be far away (route
 *    segments, the return tether) is clipped in doubles before it is narrowed.
 */

typedef struct {
    lv_area_t content;
    int32_t   px, py;           /* the pivot: centre of the view, integer pixel */
    double    center_wx;        /* world pixel shown at the pivot */
    double    center_wy;
    double    cosr, sinr;       /* rotation, as bearing-up */
    int       angle01;          /* bearing-up in 0.1 deg; 0 means north up */
    double    rot_deg;          /* the same, in degrees */
} render_ctx_t;

static void mv_make_ctx(map_view_t *mv, render_ctx_t *ctx)
{
    mv_content_area(mv, &ctx->content);
    ctx->px = ctx->content.x1 + lv_area_get_width(&ctx->content) / 2;
    ctx->py = ctx->content.y1 + lv_area_get_height(&ctx->content) / 2;
    ctx->center_wx = mv->center_wx;
    ctx->center_wy = mv->center_wy;
    ctx->angle01 = mv_angle01(mv);
    ctx->rot_deg = ctx->angle01 / 10.0;
    mv_rot_trig(mv, &ctx->cosr, &ctx->sinr);
}

/* Screen = pivot + R(-theta) * (world - centre), theta = bearing that points up. */
static inline void mv_world_to_screen(const render_ctx_t *ctx, double wx, double wy,
                                      double *sx, double *sy)
{
    double dx = wx - ctx->center_wx;
    double dy = wy - ctx->center_wy;
    *sx = ctx->px + dx * ctx->cosr + dy * ctx->sinr;
    *sy = ctx->py - dx * ctx->sinr + dy * ctx->cosr;
}

static inline void mv_screen_to_world(const render_ctx_t *ctx, double sx, double sy,
                                      double *wx, double *wy)
{
    double x = sx - ctx->px;
    double y = sy - ctx->py;
    *wx = ctx->center_wx + x * ctx->cosr - y * ctx->sinr;
    *wy = ctx->center_wy + x * ctx->sinr + y * ctx->cosr;
}

/** Bounding box, in world pixels, of everything the screen can show. */
static void mv_world_bounds(const render_ctx_t *ctx, double margin,
                            double *x0, double *y0, double *x1, double *y1)
{
    const double cx[4] = { ctx->content.x1, ctx->content.x2, ctx->content.x1, ctx->content.x2 };
    const double cy[4] = { ctx->content.y1, ctx->content.y1, ctx->content.y2, ctx->content.y2 };
    double lo_x = 1e300, lo_y = 1e300, hi_x = -1e300, hi_y = -1e300;
    for (int i = 0; i < 4; i++) {
        double wx, wy;
        mv_screen_to_world(ctx, cx[i], cy[i], &wx, &wy);
        if (wx < lo_x) lo_x = wx;
        if (wx > hi_x) hi_x = wx;
        if (wy < lo_y) lo_y = wy;
        if (wy > hi_y) hi_y = wy;
    }
    *x0 = lo_x - margin;  *y0 = lo_y - margin;
    *x1 = hi_x + margin;  *y1 = hi_y + margin;
}

static void mv_draw_tiles(map_view_t *mv, lv_draw_ctx_t *dc, const render_ctx_t *ctx)
{
    const int  n       = 1 << mv->zoom;
    const bool rotated = ctx->angle01 != 0;

    double bx0, by0, bx1, by1;
    mv_world_bounds(ctx, rotated ? 2.0 : 0.0, &bx0, &by0, &bx1, &by1);
    int tx0 = (int)floor(bx0 / (double)TILE_PX);
    int ty0 = (int)floor(by0 / (double)TILE_PX);
    int tx1 = (int)floor(bx1 / (double)TILE_PX);
    int ty1 = (int)floor(by1 / (double)TILE_PX);

    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);

    lv_draw_rect_dsc_t hole_dsc;
    lv_draw_rect_dsc_init(&hole_dsc);
    hole_dsc.bg_color = lv_color_hex(0xE8E4DD);   /* neutral "paper" while loading */
    hole_dsc.bg_opa   = LV_OPA_COVER;
    hole_dsc.border_width = 0;

    if (rotated) {
        /* Under a rotated map a missing tile would need a rotated polygon, and
         * smoothed tile edges blend with whatever is underneath. One flat fill up
         * front handles both: holes are simply "not drawn", seams show paper. */
        lv_draw_rect(dc, &hole_dsc, &ctx->content);

        /* LVGL rotates clockwise; the map turns the opposite way to the bearing. */
        img_dsc.angle     = (int16_t)((3600 - ctx->angle01) % 3600);
        img_dsc.zoom      = LV_IMG_ZOOM_NONE;
        img_dsc.antialias = 0;
    }

    /* Smooth rotation = a hard-edged pass first, then the bilinear pass on top. Drawn
     * alone, a smoothed tile fades out at its border and lets the paper show through
     * as a faint seam; over the hard pass the fade blends with the neighbouring tile. */
    const int passes = (rotated && mv->smooth_rot) ? 2 : 1;

    int vis_tx0 = tx1 + 1, vis_ty0 = ty1 + 1, vis_tx1 = tx0 - 1, vis_ty1 = ty0 - 1;

    /* Deliberately no clip-area culling. LVGL calls DRAW_MAIN once per
     * partial-buffer chunk, and every call is a "frame" to the cache: each must
     * touch all visible tiles or the ones it skipped become eviction candidates. */
    for (int pass = 0; pass < passes; pass++) {
    img_dsc.antialias = (pass == 1);
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            /* Integer offsets from the pivot, so neighbours are exactly 256 px apart
             * and a rigid rotation about the pivot leaves no gaps between them. */
            int32_t ox = (int32_t)lround(tx * (double)TILE_PX - ctx->center_wx);
            int32_t oy = (int32_t)lround(ty * (double)TILE_PX - ctx->center_wy);
            int32_t sx = ctx->px + ox;
            int32_t sy = ctx->py + oy;

            if (rotated) {
                /* Skip tiles whose circumscribed circle misses the screen. */
                double dx = ox + (TILE_PX - 1) / 2.0, dy = oy + (TILE_PX - 1) / 2.0;
                double cx = ctx->px + dx * ctx->cosr + dy * ctx->sinr;
                double cy = ctx->py - dx * ctx->sinr + dy * ctx->cosr;
                double gx = fmax(fmax(ctx->content.x1 - cx, cx - ctx->content.x2), 0.0);
                double gy = fmax(fmax(ctx->content.y1 - cy, cy - ctx->content.y2), 0.0);
                if (gx * gx + gy * gy > TILE_CIRCUMRADIUS * TILE_CIRCUMRADIUS) continue;
            }

            lv_area_t area;
            area.x1 = (lv_coord_t)sx;
            area.y1 = (lv_coord_t)sy;
            area.x2 = (lv_coord_t)(sx + TILE_PX - 1);
            area.y2 = (lv_coord_t)(sy + TILE_PX - 1);

            /* Above or below the world: nothing exists there. */
            if (ty < 0 || ty >= n) {
                if (!rotated) lv_draw_rect(dc, &hole_dsc, &area);
                continue;
            }

            if (tx < vis_tx0) vis_tx0 = tx;
            if (tx > vis_tx1) vis_tx1 = tx;
            if (ty < vis_ty0) vis_ty0 = ty;
            if (ty > vis_ty1) vis_ty1 = ty;

            int wrapped_tx = ((tx % n) + n) % n;
            lv_img_dsc_t *img = map_tile_cache_get(mv->cache, mv->zoom, wrapped_tx, ty, true);
            if (img) {
                if (rotated) {
                    /* Every tile turns about the same point, the centre of the view. */
                    img_dsc.pivot.x = (lv_coord_t)(ctx->px - sx);
                    img_dsc.pivot.y = (lv_coord_t)(ctx->py - sy);
                }
                lv_draw_img(dc, &img_dsc, &area, img);
            } else if (!rotated) {
                lv_draw_rect(dc, &hole_dsc, &area);
            }
        }
    }
    }   /* passes */

    if (vis_tx1 < vis_tx0 || vis_ty1 < vis_ty0) return;      /* nothing on screen */

    /* Warm the ring just outside what is shown so a pan finds it already there.
     * Only worth doing when the cache can hold the ring as well as the screen,
     * otherwise the ring would evict the tiles we are about to draw. */
    int span_x = vis_tx1 - vis_tx0 + 1;
    int span_y = vis_ty1 - vis_ty0 + 1;
    map_tile_cache_stats_t st;
    map_tile_cache_get_stats(mv->cache, &st);
    if (st.capacity >= (span_x + 2) * (span_y + 2)) {
        for (int ty = vis_ty0 - 1; ty <= vis_ty1 + 1; ty++) {
            if (ty < 0 || ty >= n) continue;
            for (int tx = vis_tx0 - 1; tx <= vis_tx1 + 1; tx++) {
                if (tx >= vis_tx0 && tx <= vis_tx1 && ty >= vis_ty0 && ty <= vis_ty1) continue;
                map_tile_cache_prefetch(mv->cache, mv->zoom, ((tx % n) + n) % n, ty);
            }
        }
    }
}

/** Cheap reject: does the box touch the drawing area? */
static bool mv_segment_visible(const lv_area_t *content, double x1, double y1,
                               double x2, double y2, int32_t margin)
{
    double lo_x = x1 < x2 ? x1 : x2;
    double hi_x = x1 < x2 ? x2 : x1;
    double lo_y = y1 < y2 ? y1 : y2;
    double hi_y = y1 < y2 ? y2 : y1;

    if (hi_x < content->x1 - margin) return false;
    if (lo_x > content->x2 + margin) return false;
    if (hi_y < content->y1 - margin) return false;
    if (lo_y > content->y2 + margin) return false;
    return true;
}

/**
 * Liang-Barsky: clip the segment to the content area grown by @p margin.
 * Endpoints are rewritten in place. Returns false when nothing is visible.
 */
static bool mv_clip_segment(const lv_area_t *content, double *x1, double *y1,
                            double *x2, double *y2, double margin)
{
    const double xmin = content->x1 - margin, xmax = content->x2 + margin;
    const double ymin = content->y1 - margin, ymax = content->y2 + margin;

    const double ox = *x1, oy = *y1;
    const double dx = *x2 - ox, dy = *y2 - oy;
    const double p[4] = { -dx, dx, -dy, dy };
    const double q[4] = { ox - xmin, xmax - ox, oy - ymin, ymax - oy };

    double t0 = 0.0, t1 = 1.0;
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) return false;       /* parallel and outside */
        } else {
            double r = q[i] / p[i];
            if (p[i] < 0.0) { if (r > t1) return false; if (r > t0) t0 = r; }
            else            { if (r < t0) return false; if (r < t1) t1 = r; }
        }
    }

    *x1 = ox + t0 * dx;  *y1 = oy + t0 * dy;
    *x2 = ox + t1 * dx;  *y2 = oy + t1 * dy;
    return true;
}

/**
 * Draw the route polyline.
 *
 * Called twice: once for the dark casing and once for the coloured core. Two
 * full passes rather than casing+core per segment, because otherwise the next
 * segment's casing paints over the previous segment's core at every joint.
 */
static int mv_draw_route_pass(map_view_t *mv, lv_draw_ctx_t *dc, const render_ctx_t *ctx,
                              bool casing)
{
    int32_t width = casing ? mv->style.casing_width : mv->style.route_width;
    if (width <= 0) return 0;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.width       = (lv_coord_t)width;
    dsc.opa         = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end   = 1;

    int drawn = 0;

    double prev_sx = 0, prev_sy = 0;
    uint32_t prev_cum = 0;
    bool have_prev = false;

    int idx = map_route_next_lod_point(mv->route, 0, mv->zoom);
    int last_index = map_route_get_point_count(mv->route) - 1;

    while (idx >= 0) {
        map_route_point_t p;
        if (!map_route_get_point(mv->route, idx, &p)) break;

        double wx = map_geo_lon_to_world_px(p.lon, mv->zoom);
        double wy = map_geo_lat_to_world_py(p.lat, mv->zoom);
        double sx, sy;
        mv_world_to_screen(ctx, wx, wy, &sx, &sy);

        if (!have_prev) {
            prev_sx = sx;
            prev_sy = sy;
            prev_cum = p.cum_dist_m;
            have_prev = true;
        } else {
            bool is_last = (idx == last_index);
            double step = fabs(sx - prev_sx) + fabs(sy - prev_sy);

            /* Decimate against the last *anchor*, so a dense curve collapses to
             * a few segments without the polyline drifting off the road. */
            if (step >= ROUTE_MIN_STEP_PX || is_last) {
                double ax = prev_sx, ay = prev_sy, bx = sx, by = sy;
                if (mv_clip_segment(&ctx->content, &ax, &ay, &bx, &by,
                                    (double)width + MV_LINE_CLIP_MARGIN)) {
                    if (!casing) {
                        bool done = (prev_cum < mv->traveled_m);
                        dsc.color = done ? mv->style.route_done_color : mv->style.route_color;
                    } else {
                        dsc.color = mv->style.route_casing_color;
                    }
                    lv_point_t p1 = { (lv_coord_t)lround(ax), (lv_coord_t)lround(ay) };
                    lv_point_t p2 = { (lv_coord_t)lround(bx), (lv_coord_t)lround(by) };
                    lv_draw_line(dc, &dsc, &p1, &p2);

                    if (++drawn >= ROUTE_MAX_SEGMENTS) break;
                }
                prev_sx = sx;
                prev_sy = sy;
                prev_cum = p.cum_dist_m;
            }
        }

        idx = map_route_next_lod_point(mv->route, idx + 1, mv->zoom);
    }

    return drawn;
}

static void mv_draw_destination(map_view_t *mv, lv_draw_ctx_t *dc, const render_ctx_t *ctx)
{
    double lat, lon;
    if (!map_route_get_destination(mv->route, &lat, &lon)) return;

    double sx, sy;
    mv_world_to_screen(ctx,
                       map_geo_lon_to_world_px(lon, mv->zoom),
                       map_geo_lat_to_world_py(lat, mv->zoom),
                       &sx, &sy);

    int32_t r = mv->style.marker_radius;
    if (!mv_segment_visible(&ctx->content, sx - r, sy - r, sx + r, sy + r, 0)) return;

    int32_t cx = (int32_t)lround(sx), cy = (int32_t)lround(sy);
    lv_area_t a;
    a.x1 = (lv_coord_t)(cx - r);
    a.y1 = (lv_coord_t)(cy - r);
    a.x2 = (lv_coord_t)(cx + r);
    a.y2 = (lv_coord_t)(cy + r);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius       = LV_RADIUS_CIRCLE;
    dsc.bg_color     = mv->style.destination_color;
    dsc.bg_opa       = LV_OPA_COVER;
    dsc.border_color = lv_color_white();
    dsc.border_width = 3;
    dsc.border_opa   = LV_OPA_COVER;
    lv_draw_rect(dc, &dsc, &a);
}

static void mv_fill_triangle(lv_draw_ctx_t *dc, lv_color_t color,
                             double x0, double y0, double x1, double y1, double x2, double y2)
{
    lv_draw_rect_dsc_t tri;
    lv_draw_rect_dsc_init(&tri);
    tri.bg_color     = color;
    tri.bg_opa       = LV_OPA_COVER;
    tri.border_width = 0;

    lv_point_t pts[3] = {
        { (lv_coord_t)lround(x0), (lv_coord_t)lround(y0) },
        { (lv_coord_t)lround(x1), (lv_coord_t)lround(y1) },
        { (lv_coord_t)lround(x2), (lv_coord_t)lround(y2) },
    };
    lv_draw_polygon(dc, &tri, pts, 3);
}

/**
 * The tether back to the route, and an arrow when the road is off screen.
 *
 * Drawn above the route so it reads as an instruction rather than another road,
 * and dashed so it is obviously not one.
 */
static void mv_draw_return(map_view_t *mv, lv_draw_ctx_t *dc, const render_ctx_t *ctx)
{
    if (!mv->return_active || !mv->pos_valid) return;

    double fx, fy, tx, ty;
    mv_world_to_screen(ctx,
                       map_geo_lon_to_world_px(mv->pos_lon, mv->zoom),
                       map_geo_lat_to_world_py(mv->pos_lat, mv->zoom), &fx, &fy);
    mv_world_to_screen(ctx,
                       map_geo_lon_to_world_px(mv->return_lon, mv->zoom),
                       map_geo_lat_to_world_py(mv->return_lat, mv->zoom), &tx, &ty);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color      = mv->style.return_color;
    dsc.width      = 5;
    dsc.opa        = LV_OPA_COVER;
    dsc.dash_width = 12;
    dsc.dash_gap   = 9;
    dsc.round_start = 1;
    dsc.round_end   = 1;

    double lx1 = fx, ly1 = fy, lx2 = tx, ly2 = ty;
    if (mv_clip_segment(&ctx->content, &lx1, &ly1, &lx2, &ly2, 5.0 + MV_LINE_CLIP_MARGIN)) {
        lv_point_t p1 = { (lv_coord_t)lround(lx1), (lv_coord_t)lround(ly1) };
        lv_point_t p2 = { (lv_coord_t)lround(lx2), (lv_coord_t)lround(ly2) };
        lv_draw_line(dc, &dsc, &p1, &p2);
    }

    /* If the road is off screen the dashes run off the edge and say nothing
     * about how far. Put an arrow where the line leaves the view. */
    bool on_screen = tx >= ctx->content.x1 && tx <= ctx->content.x2 &&
                     ty >= ctx->content.y1 && ty <= ctx->content.y2;
    if (on_screen) return;

    double cx = (ctx->content.x1 + ctx->content.x2) / 2.0;
    double cy = (ctx->content.y1 + ctx->content.y2) / 2.0;
    double dx = tx - cx, dy = ty - cy;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1.0) return;
    dx /= len;
    dy /= len;

    /* The panel may be round, so the edge is a circle inset by the arrow's reach
     * rather than the corner of a rectangle. */
    double radius = (lv_area_get_width(&ctx->content) < lv_area_get_height(&ctx->content)
                     ? lv_area_get_width(&ctx->content)
                     : lv_area_get_height(&ctx->content)) / 2.0 - MV_EDGE_ARROW_INSET;
    double ax = cx + dx * radius;
    double ay = cy + dy * radius;
    double px = -dy, py = dx;              /* perpendicular, for the base */

    mv_fill_triangle(dc, mv->style.return_color,
                     ax + dx * 16,            ay + dy * 16,
                     ax - dx * 8 + px * 12,   ay - dy * 8 + py * 12,
                     ax - dx * 8 - px * 12,   ay - dy * 8 - py * 12);
}

static void mv_draw_position(map_view_t *mv, lv_draw_ctx_t *dc, const render_ctx_t *ctx)
{
    if (!mv->pos_valid) return;

    double sx, sy;
    mv_world_to_screen(ctx,
                       map_geo_lon_to_world_px(mv->pos_lon, mv->zoom),
                       map_geo_lat_to_world_py(mv->pos_lat, mv->zoom),
                       &sx, &sy);

    int32_t r = mv->style.marker_radius;
    lv_color_t color = mv->off_route ? mv->style.marker_off_color : mv->style.marker_color;

    if (!mv_segment_visible(&ctx->content, sx - r - 20, sy - r - 20, sx + r + 20, sy + r + 20, 0)) {
        return;
    }
    int32_t cx = (int32_t)lround(sx);
    int32_t cy = (int32_t)lround(sy);

    /* Heading arrow first, so the dot sits on top of its base. */
    double th   = (mv->pos_heading - ctx->rot_deg) * M_PI / 180.0;   /* on-screen direction */
    double dirx = sin(th);
    double diry = -cos(th);
    double perx = cos(th);
    double pery = sin(th);

    mv_fill_triangle(dc, color,
                     cx + dirx * (r + 15),              cy + diry * (r + 15),
                     cx + dirx * (r + 1) + perx * 9,    cy + diry * (r + 1) + pery * 9,
                     cx + dirx * (r + 1) - perx * 9,    cy + diry * (r + 1) - pery * 9);

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius       = LV_RADIUS_CIRCLE;
    dot.bg_color     = color;
    dot.bg_opa       = LV_OPA_COVER;
    dot.border_color = lv_color_white();
    dot.border_width = 3;
    dot.border_opa   = LV_OPA_COVER;

    lv_area_t a;
    a.x1 = (lv_coord_t)(cx - r);
    a.y1 = (lv_coord_t)(cy - r);
    a.x2 = (lv_coord_t)(cx + r);
    a.y2 = (lv_coord_t)(cy + r);
    lv_draw_rect(dc, &dot, &a);
}

static void mv_draw_debug(map_view_t *mv, lv_draw_ctx_t *dc, const render_ctx_t *ctx,
                          int route_segments)
{
    map_tile_cache_stats_t st;
    map_tile_cache_get_stats(mv->cache, &st);

    char buf[96];
    snprintf(buf, sizeof(buf), "z%d rot%.0f  tiles %d/%d ld%d  hit %lu/%lu  seg %d",
             mv->zoom, ctx->rot_deg, st.resident, st.capacity, st.loading,
             (unsigned long)st.hits, (unsigned long)(st.hits + st.misses),
             route_segments);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = lv_color_white();
    dsc.opa   = LV_OPA_COVER;
    dsc.align = LV_TEXT_ALIGN_CENTER;

    lv_area_t a;
    a.x1 = ctx->content.x1;
    a.y1 = (lv_coord_t)(ctx->content.y1 + 4);
    a.x2 = ctx->content.x2;
    a.y2 = (lv_coord_t)(ctx->content.y1 + 24);

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_black();
    bg.bg_opa   = LV_OPA_50;
    bg.border_width = 0;
    lv_draw_rect(dc, &bg, &a);

    /* LVGL 8 draws labels immediately, so the stack buffer is fine. */
    lv_draw_label(dc, &dsc, &a, buf, NULL);
}

static void mv_draw(map_view_t *mv, lv_event_t *e)
{
    if (!mv->cache) return;

    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    if (!dc) return;

    render_ctx_t ctx;
    mv_make_ctx(mv, &ctx);

    map_tile_cache_begin_frame(mv->cache);
    mv_draw_tiles(mv, dc, &ctx);

    int segments = 0;
    if (mv->route) {
        mv_draw_route_pass(mv, dc, &ctx, true);
        segments = mv_draw_route_pass(mv, dc, &ctx, false);
        mv_draw_destination(mv, dc, &ctx);

        if (segments >= ROUTE_MAX_SEGMENTS && !mv->warned_segment_cap) {
            mv->warned_segment_cap = true;
            MAP_LOGW(TAG, "Route hit the %d segment cap; raise the exporter's "
                          "simplification tolerance for this zoom", ROUTE_MAX_SEGMENTS);
        }
    }

    mv_draw_return(mv, dc, &ctx);
    mv_draw_position(mv, dc, &ctx);

    if (mv->debug) mv_draw_debug(mv, dc, &ctx, segments);
}

/* ----------------------------------------------------------------- input */

static void mv_user_took_over(map_view_t *mv)
{
    if (mv->follow) {
        mv->follow = false;
        lv_event_send(mv->obj, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

static void mv_handle_input(map_view_t *mv, lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        mv->pressing     = true;
        mv->drag_active  = false;
        mv->press_travel = 0;
        mv->vel_x = mv->vel_y = 0;
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;

        lv_point_t vect;
        lv_indev_get_vect(indev, &vect);
        if (vect.x == 0 && vect.y == 0) return;

        mv->press_travel += LV_ABS(vect.x) + LV_ABS(vect.y);
        if (!mv->drag_active && mv->press_travel < DRAG_START_PX) return;

        if (!mv->drag_active) {
            mv->drag_active = true;
            mv_user_took_over(mv);
        }

        /* Dragging moves the map with the finger, so the centre goes the
         * opposite way. */
        double c, s;
        mv_rot_trig(mv, &c, &s);
        double wdx = vect.x * c - vect.y * s;       /* the finger's move, in world pixels */
        double wdy = vect.x * s + vect.y * c;
        mv->center_wx -= wdx;
        mv->center_wy -= wdy;
        mv_clamp_center(mv);

        mv->vel_x = -wdx;
        mv->vel_y = -wdy;

        lv_obj_invalidate(mv->obj);
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        mv->pressing = false;
        if (!mv->drag_active) {
            mv->vel_x = mv->vel_y = 0;
        }
        mv->drag_active = false;
        return;
    }
}

/* ------------------------------------------------------------- refreshing */

static void mv_timer_cb(lv_timer_t *t)
{
    map_view_t *mv = (map_view_t *)t->user_data;
    if (!mv) return;

    bool dirty = false;

    /* A background tile arrived: repaint. */
    if (mv->cache) {
        uint32_t v = map_tile_cache_get_version(mv->cache);
        if (v != mv->cache_version) {
            mv->cache_version = v;
            dirty = true;
        }
    }

    /* Fling after a drag. */
    if (!mv->pressing && (fabs(mv->vel_x) > MOMENTUM_STOP_PX || fabs(mv->vel_y) > MOMENTUM_STOP_PX)) {
        mv->center_wx += mv->vel_x;
        mv->center_wy += mv->vel_y;
        mv->vel_x *= MOMENTUM_DECAY;
        mv->vel_y *= MOMENTUM_DECAY;
        mv_clamp_center(mv);
        dirty = true;
    }

    /* Heading-up: the bearing that points up chases the vehicle's heading, but only
     * while following, so panning away does not make the map spin under the finger. */
    if (mv->heading_up && mv->follow && mv->pos_valid) {
        mv->rot_target = mv_norm_deg(mv->pos_heading);
    }
    double turn = mv_turn_deg(mv->rot_target, mv->rot_deg);
    if (fabs(turn) > 1e-9) {
        int before = mv_angle01(mv);
        mv->rot_deg = (fabs(turn) < ROTATE_SNAP_DEG) ? mv->rot_target
                                                     : mv_norm_deg(mv->rot_deg + turn * ROTATE_EASE);
        if (mv_angle01(mv) != before) dirty = true;
    }

    /* Ease toward the vehicle instead of snapping, so a jittery fix does not
     * make the whole map twitch. */
    if (mv->follow) {
        double tx, ty;
        if (mv_follow_target(mv, &tx, &ty)) {
            double dx = tx - mv->center_wx;
            double dy = ty - mv->center_wy;
            if (fabs(dx) > FOLLOW_SNAP_PX || fabs(dy) > FOLLOW_SNAP_PX) {
                mv->center_wx += dx * FOLLOW_EASE;
                mv->center_wy += dy * FOLLOW_EASE;
                mv_clamp_center(mv);
                dirty = true;
            } else if (dx != 0.0 || dy != 0.0) {
                mv->center_wx = tx;
                mv->center_wy = ty;
                mv_clamp_center(mv);
                dirty = true;
            }
        }
    }

    if (dirty) lv_obj_invalidate(mv->obj);
}

static void mv_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    lv_event_code_t code = lv_event_get_code(e);

    switch (code) {
        case LV_EVENT_DRAW_MAIN:
            mv_draw(mv, e);
            break;

        case LV_EVENT_PRESSED:
        case LV_EVENT_PRESSING:
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            mv_handle_input(mv, e);
            break;

        case LV_EVENT_DELETE:
            if (mv->timer) lv_timer_del(mv->timer);
            if (mv->cache) map_tile_cache_destroy(mv->cache);
            lv_obj_set_user_data(obj, NULL);
            free(mv);
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------ public API */

lv_obj_t *map_view_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) return NULL;

    map_view_t *mv = (map_view_t *)calloc(1, sizeof(map_view_t));
    if (!mv) {
        MAP_LOGE(TAG, "Out of memory for the map view");
        lv_obj_del(obj);
        return NULL;
    }

    mv->obj      = obj;
    mv->zoom     = 16;
    mv->min_zoom = 10;
    mv->max_zoom = 19;
    mv->follow_offset = 0;
    mv->rot_deg = mv->rot_target = 0.0;
    mv->heading_up = false;
    mv->smooth_rot = false;
    mv_default_style(&mv->style);

    /* Start on the prime meridian at the equator; the app will move us. */
    mv->center_wx = map_geo_lon_to_world_px(0.0, mv->zoom);
    mv->center_wy = map_geo_lat_to_world_py(0.0, mv->zoom);

    lv_obj_set_user_data(obj, mv);

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xE8E4DD), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    lv_obj_add_event_cb(obj, mv_event_cb, LV_EVENT_ALL, NULL);

    mv->timer = lv_timer_create(mv_timer_cb, REFRESH_PERIOD_MS, mv);

    return obj;
}

bool map_view_set_tile_source(lv_obj_t *obj, const char *base_path, const char *folder,
                              int cache_capacity, bool use_spiram)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return false;

    if (mv->cache) {
        map_tile_cache_destroy(mv->cache);
        mv->cache = NULL;
    }

    map_tile_cache_config_t cfg = {
        .base_path  = base_path,
        .folder     = folder,
        .capacity   = cache_capacity,
        .use_spiram = use_spiram,
    };
    mv->cache = map_tile_cache_create(&cfg);
    if (!mv->cache) return false;

    mv->cache_version = 0;
    lv_obj_invalidate(obj);
    return true;
}

void map_view_set_tile_folder(lv_obj_t *obj, const char *folder)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || !mv->cache) return;
    map_tile_cache_set_folder(mv->cache, folder);
    lv_obj_invalidate(obj);
}

map_tile_cache_handle_t map_view_get_tile_cache(lv_obj_t *obj)
{
    map_view_t *mv = mv_of(obj);
    return mv ? mv->cache : NULL;
}

void map_view_set_zoom(lv_obj_t *obj, int zoom)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    if (zoom < mv->min_zoom) zoom = mv->min_zoom;
    if (zoom > mv->max_zoom) zoom = mv->max_zoom;
    if (zoom == mv->zoom) return;

    mv_reproject_center(mv, zoom);
    mv->vel_x = mv->vel_y = 0;
    mv->warned_segment_cap = false;
    lv_obj_invalidate(obj);
}

int map_view_get_zoom(lv_obj_t *obj)
{
    map_view_t *mv = mv_of(obj);
    return mv ? mv->zoom : 0;
}

void map_view_set_zoom_range(lv_obj_t *obj, int min_zoom, int max_zoom)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || min_zoom > max_zoom) return;

    mv->min_zoom = min_zoom;
    mv->max_zoom = max_zoom;
    map_view_set_zoom(obj, mv->zoom);
}

void map_view_zoom_in(lv_obj_t *obj)  { map_view_set_zoom(obj, map_view_get_zoom(obj) + 1); }
void map_view_zoom_out(lv_obj_t *obj) { map_view_set_zoom(obj, map_view_get_zoom(obj) - 1); }

void map_view_set_center(lv_obj_t *obj, double lat, double lon)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    mv->center_wx = map_geo_lon_to_world_px(lon, mv->zoom);
    mv->center_wy = map_geo_lat_to_world_py(lat, mv->zoom);
    mv->vel_x = mv->vel_y = 0;
    mv_clamp_center(mv);
    lv_obj_invalidate(obj);
}

void map_view_get_center(lv_obj_t *obj, double *lat, double *lon)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;
    if (lat) *lat = map_geo_world_py_to_lat(mv->center_wy, mv->zoom);
    if (lon) *lon = map_geo_world_px_to_lon(mv->center_wx, mv->zoom);
}

void map_view_fit_bounds(lv_obj_t *obj,
                         double min_lat, double min_lon,
                         double max_lat, double max_lon,
                         int padding_px)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    lv_area_t content;
    mv_content_area(mv, &content);
    int32_t w = lv_area_get_width(&content) - 2 * padding_px;
    int32_t h = lv_area_get_height(&content) - 2 * padding_px;
    if (w < 16) w = 16;
    if (h < 16) h = 16;

    int best = mv->min_zoom;
    for (int z = mv->max_zoom; z >= mv->min_zoom; z--) {
        double x1 = map_geo_lon_to_world_px(min_lon, z);
        double x2 = map_geo_lon_to_world_px(max_lon, z);
        double y1 = map_geo_lat_to_world_py(max_lat, z);   /* north = smaller y */
        double y2 = map_geo_lat_to_world_py(min_lat, z);

        /* With the map turned, the box's footprint on screen is its rotated extent. */
        double rc, rs;
        mv_rot_trig(mv, &rc, &rs);
        double ex = fabs(x2 - x1) * fabs(rc) + fabs(y2 - y1) * fabs(rs);
        double ey = fabs(x2 - x1) * fabs(rs) + fabs(y2 - y1) * fabs(rc);

        if (ex <= w && ey <= h) {
            best = z;
            break;
        }
    }

    mv->zoom = best;
    mv->center_wx = (map_geo_lon_to_world_px(min_lon, best) +
                     map_geo_lon_to_world_px(max_lon, best)) / 2.0;
    mv->center_wy = (map_geo_lat_to_world_py(min_lat, best) +
                     map_geo_lat_to_world_py(max_lat, best)) / 2.0;
    mv->vel_x = mv->vel_y = 0;
    mv_clamp_center(mv);
    lv_obj_invalidate(obj);
}

void map_view_set_route(lv_obj_t *obj, map_route_handle_t route)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;
    mv->route = route;
    mv->traveled_m = 0;
    mv->warned_segment_cap = false;
    lv_obj_invalidate(obj);
}

void map_view_set_progress(lv_obj_t *obj, uint32_t traveled_m)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || mv->traveled_m == traveled_m) return;
    mv->traveled_m = traveled_m;
    lv_obj_invalidate(obj);
}

void map_view_set_position(lv_obj_t *obj, double lat, double lon,
                           double heading_deg, bool valid)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    if (mv->pos_valid == valid && mv->pos_lat == lat && mv->pos_lon == lon &&
        mv->pos_heading == heading_deg) {
        return;   /* same fix replayed; nothing to redraw */
    }

    mv->pos_lat     = lat;
    mv->pos_lon     = lon;
    mv->pos_heading = heading_deg;
    mv->pos_valid   = valid;
    lv_obj_invalidate(obj);
}

void map_view_set_off_route(lv_obj_t *obj, bool off_route)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || mv->off_route == off_route) return;
    mv->off_route = off_route;
    lv_obj_invalidate(obj);
}

void map_view_set_return_target(lv_obj_t *obj, double lat, double lon, bool active)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    if (mv->return_active == active &&
        mv->return_lat == lat && mv->return_lon == lon) {
        return;                 /* same target replayed; nothing to redraw */
    }

    mv->return_active = active;
    mv->return_lat    = lat;
    mv->return_lon    = lon;
    lv_obj_invalidate(obj);
}

void map_view_set_follow(lv_obj_t *obj, bool follow)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || mv->follow == follow) return;

    mv->follow = follow;
    if (follow) {
        mv->vel_x = mv->vel_y = 0;
    }
    lv_event_send(obj, LV_EVENT_VALUE_CHANGED, NULL);
}

bool map_view_get_follow(lv_obj_t *obj)
{
    map_view_t *mv = mv_of(obj);
    return mv ? mv->follow : false;
}

void map_view_set_follow_offset(lv_obj_t *obj, int32_t offset_px)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;
    mv->follow_offset = offset_px;
}

void map_view_set_rotation(lv_obj_t *obj, double bearing_up_deg)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;
    mv->heading_up = false;                 /* a manual angle sticks until heading-up is asked for */
    mv->rot_deg = mv->rot_target = mv_norm_deg(bearing_up_deg);
    lv_obj_invalidate(obj);
}

double map_view_get_rotation(lv_obj_t *obj)
{
    map_view_t *mv = mv_of(obj);
    return mv ? mv_angle01(mv) / 10.0 : 0.0;
}

void map_view_set_heading_up(lv_obj_t *obj, bool enabled)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || mv->heading_up == enabled) return;
    mv->heading_up = enabled;
    if (enabled) {
        if (mv->pos_valid) mv->rot_target = mv_norm_deg(mv->pos_heading);
    } else {
        mv->rot_target = 0.0;               /* ease back to north-up */
    }
}

bool map_view_get_heading_up(lv_obj_t *obj)
{
    map_view_t *mv = mv_of(obj);
    return mv && mv->heading_up;
}

void map_view_set_smooth_rotation(lv_obj_t *obj, bool smooth)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || mv->smooth_rot == smooth) return;
    mv->smooth_rot = smooth;
    lv_obj_invalidate(obj);
}

void map_view_set_style(lv_obj_t *obj, const map_view_style_t *style)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || !style) return;
    mv->style = *style;
    lv_obj_invalidate(obj);
}

void map_view_get_style(lv_obj_t *obj, map_view_style_t *out)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || !out) return;
    *out = mv->style;
}

void map_view_set_debug_overlay(lv_obj_t *obj, bool enabled)
{
    map_view_t *mv = mv_of(obj);
    if (!mv || mv->debug == enabled) return;
    mv->debug = enabled;
    lv_obj_invalidate(obj);
}

void map_view_screen_to_latlon(lv_obj_t *obj, int32_t x, int32_t y, double *lat, double *lon)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return;

    render_ctx_t ctx;
    mv_make_ctx(mv, &ctx);

    double wx, wy;
    mv_screen_to_world(&ctx, x, y, &wx, &wy);

    if (lat) *lat = map_geo_world_py_to_lat(wy, mv->zoom);
    if (lon) *lon = map_geo_world_px_to_lon(wx, mv->zoom);
}

bool map_view_latlon_to_screen(lv_obj_t *obj, double lat, double lon, int32_t *x, int32_t *y)
{
    map_view_t *mv = mv_of(obj);
    if (!mv) return false;

    render_ctx_t ctx;
    mv_make_ctx(mv, &ctx);

    double sx, sy;
    mv_world_to_screen(&ctx,
                       map_geo_lon_to_world_px(lon, mv->zoom),
                       map_geo_lat_to_world_py(lat, mv->zoom),
                       &sx, &sy);

    if (x) *x = (int32_t)lround(sx);
    if (y) *y = (int32_t)lround(sy);

    return sx >= ctx.content.x1 && sx <= ctx.content.x2 &&
           sy >= ctx.content.y1 && sy <= ctx.content.y2;
}
