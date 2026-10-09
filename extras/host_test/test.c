#include <stdio.h>
#include <unistd.h>
#include "lvgl.h"
#include "MapTiles.h"

#define W 480
#define H 480
static uint16_t fb[W * H];
static lv_color_t buf1[W * 48];
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;
static lv_point_t ptr = {0, 0};
static lv_indev_state_t ptr_state = LV_INDEV_STATE_RELEASED;
static int flush_calls = 0;

static void flush_cb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c)
{
    flush_calls++;
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++) {
#if LV_COLOR_DEPTH == 16
#if LV_COLOR_16_SWAP
            fb[y * W + x] = (uint16_t)((c->full >> 8) | (c->full << 8));   /* undo the display swap to inspect */
#else
            fb[y * W + x] = c->full;
#endif
#else
            uint32_t v = c->full; uint8_t r=(v>>16)&255,g=(v>>8)&255,b=v&255;
            fb[y * W + x] = ((r>>3)<<11)|((g>>2)<<5)|(b>>3);
#endif
            c++;
        }
    lv_disp_flush_ready(d);
}
static void ptr_read(lv_indev_drv_t *d, lv_indev_data_t *data) { (void)d; data->point = ptr; data->state = ptr_state; }

static void spin(int ms) { for (int i = 0; i < ms / 10; i++) { lv_tick_inc(10); lv_timer_handler(); usleep(10000); } }

static void dump(const char *name)
{
    char p[128]; snprintf(p, sizeof p, "out/%s.ppm", name);
    FILE *f = fopen(p, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { uint16_t v = fb[i]; uint8_t rgb[3] = { ((v>>11)&31)<<3, ((v>>5)&63)<<2, (v&31)<<3 }; fwrite(rgb, 1, 3, f); }
    fclose(f);
}
static int fails = 0;
static int hook_opens = 0;
static void *hk_open(const char *p, void *u) { (void)u; hook_opens++; return fopen(p, "rb"); }
static size_t hk_read(void *f, void *b, size_t n, void *u) { (void)u; return fread(b, 1, n, (FILE *)f); }
static bool hk_seek(void *f, uint32_t pos, void *u) { (void)u; return fseek((FILE *)f, pos, SEEK_SET) == 0; }
static void hk_close(void *f, void *u) { (void)u; fclose((FILE *)f); }
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } else { printf("  ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int value_changed = 0;
static void vc_cb(lv_event_t *e) { (void)e; value_changed++; }
static bool near_color(uint16_t px, uint32_t hex)
{
    int r = ((px >> 11) & 31) << 3, g = ((px >> 5) & 63) << 2, b = (px & 31) << 3;
    int R = (hex >> 16) & 255, G = (hex >> 8) & 255, B = hex & 255;
    return abs(r - R) < 12 && abs(g - G) < 8 && abs(b - B) < 12;
}

int main(void)
{
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, W * 48);          /* partial buffer: 10 chunks per frame */
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = W; disp_drv.ver_res = H; disp_drv.flush_cb = flush_cb; disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    lv_indev_drv_init(&indev_drv); indev_drv.type = LV_INDEV_TYPE_POINTER; indev_drv.read_cb = ptr_read;
    lv_indev_drv_register(&indev_drv);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    printf("== route: peek / load\n");
    map_route_info_t info;
    CHECK(map_route_peek("/tmp/maptest/routes/route.bin", &info), "peek ok");
    CHECK(info.point_count == 89 && info.maneuver_count == 5, "points=%u maneuvers=%u", info.point_count, info.maneuver_count);
    CHECK(strcmp(info.name, "Test loop") == 0 && strcmp(info.tile_folder, "tiles1") == 0, "name='%s' folder='%s'", info.name, info.tile_folder);
    map_route_handle_t route = map_route_load("/tmp/maptest/routes/route.bin", true);
    CHECK(route != NULL, "load");

    printf("== map view: tiles + route + position\n");
    lv_obj_t *map = map_view_create(scr);
    lv_obj_set_size(map, W, H); lv_obj_center(map);
    lv_obj_add_event_cb(map, vc_cb, LV_EVENT_VALUE_CHANGED, NULL);
    CHECK(map_view_set_tile_source(map, "/tmp/maptest", "tiles1", 28, false), "tile source");
    int zmin, zmax;
    CHECK(map_tile_cache_probe_zooms("/tmp/maptest", "tiles1", &zmin, &zmax) == 3 && zmin == 15 && zmax == 17, "probe zooms z%d..z%d", zmin, zmax);
    map_view_set_zoom_range(map, zmin, zmax);
    map_view_set_zoom(map, 17);
    map_view_set_route(map, route);
    double slat, slon; map_route_get_start(route, &slat, &slon);
    map_route_point_t p30; map_route_get_point(route, 40, &p30);   /* on the northbound leg */
    map_route_point_t p20; map_route_get_point(route, 20, &p20);   /* before the first turn */
    map_view_set_center(map, p30.lat, p30.lon);
    map_view_set_position(map, p30.lat, p30.lon, p30.bearing_deg, true);
    map_view_set_debug_overlay(map, true);
    spin(1500);
    dump("01_route_z17");
    map_tile_cache_stats_t st; map_tile_cache_get_stats(map_view_get_tile_cache(map), &st);
    CHECK(st.resident >= 9 && st.missing == 0, "resident=%d missing=%d loads=%u evictions=%u flushes=%d", st.resident, st.missing, st.loads, st.evictions, flush_calls);
    /* the marker is at the screen centre; the grey road is under it, so the centre pixel ring must be marker blue/white */
    CHECK(near_color(fb[(H/2) * W + W/2], 0x1A73E8), "marker colour at centre = 0x%04x", fb[(H/2) * W + W/2]);
    /* route core colour (blue 2F7BEF) must exist somewhere on screen, and casing too */
    int blue = 0, casing = 0; for (int i = 0; i < W*H; i++) { if (near_color(fb[i], 0x2F7BEF)) blue++; if (near_color(fb[i], 0x123A6E)) casing++; }
    CHECK(blue > 500 && casing > 300, "route pixels core=%d casing=%d", blue, casing);
    /* alignment: where the route core is drawn, the underlying tile road is grey 787878 and must be fully covered:
       count leftover road-grey pixels within 3px of core pixels -> should be ~0 (road is 8px wide at z17, route 9px) */
    int paper = 0; for (int i = 0; i < W*H; i++) if (near_color(fb[i], 0xE8E4DD) || near_color(fb[i], 0xD6DED2)) paper++;
    CHECK(paper > 100000, "tile colours survive the loader (RGB565 -> %d-bit%s): %d paper pixels", LV_COLOR_DEPTH, LV_COLOR_16_SWAP ? " swapped" : "", paper);
    int grey_near_blue = 0;
    for (int y = 3; y < H-3; y++) for (int x = 3; x < W-3; x++) if (near_color(fb[y*W+x], 0x2F7BEF)) {
        for (int dy=-3; dy<=3; dy++) for (int dx=-3; dx<=3; dx++) if (near_color(fb[(y+dy)*W+x+dx], 0x787878)) grey_near_blue++; }
    CHECK(grey_near_blue == 0, "tile road hidden exactly under route (alignment), grey-near-core=%d", grey_near_blue);

    printf("== map matching\n");
    map_route_match_t m;
    CHECK(map_route_match(route, p20.lat, p20.lon, 35.0, &m) && m.on_route && m.cross_track_m < 1.0, "on route, xtrack=%.2f m", m.cross_track_m);
    CHECK(m.traveled_m > 480 && m.traveled_m < 520, "traveled=%u remaining=%u", m.traveled_m, m.remaining_m);
    uint32_t good_progress = m.traveled_m;
    CHECK(m.next_maneuver == 1 && m.dist_to_maneuver_m > 180 && m.dist_to_maneuver_m < 220, "next maneuver idx=%d in %u m", m.next_maneuver, m.dist_to_maneuver_m);
    map_maneuver_t mv; map_route_get_maneuver(route, m.next_maneuver, &mv);
    CHECK(mv.type == MAP_MANEUVER_TURN_LEFT && strcmp(mv.street, "North Ave") == 0, "maneuver '%s' on '%s' (%s)", map_maneuver_type_name(mv.type), mv.street, mv.instruction);
    double olon = p30.lon + 150 / (111320.0 * cos(p30.lat * M_PI / 180));   /* 150 m east of the northbound leg */
    map_route_match(route, p30.lat, olon, 35.0, &m);
    CHECK(!m.on_route && m.cross_track_m > 140 && m.cross_track_m < 160 && m.traveled_m == good_progress, "off route xtrack=%.0f m, progress frozen at %u (was %u)", m.cross_track_m, m.traveled_m, good_progress);
    CHECK(fabs(m.return_bearing_deg - 270.0) < 3.0, "road is due west of the fix: bearing %.1f", m.return_bearing_deg);
    map_view_set_center(map, p30.lat, olon);
    map_view_set_position(map, p30.lat, olon, 0, true);
    map_view_set_off_route(map, true);
    map_view_set_return_target(map, m.return_lat, m.return_lon, true);
    map_view_set_follow(map, true);
    spin(800);
    dump("02_off_route");
    int orange = 0, red = 0; for (int i = 0; i < W*H; i++) { if (near_color(fb[i], 0xFF8A3D)) orange++; if (near_color(fb[i], 0xE03B24)) red++; }
    CHECK(orange > 100 && red > 100, "return tether (orange=%d) and off-route marker (red=%d) drawn", orange, red);
    map_route_reset_match(route);
    map_route_point_t last; map_route_get_point(route, 88, &last);
    map_route_match(route, last.lat, last.lon, 35.0, &m);
    CHECK(m.arrived, "arrived at destination (remaining=%u)", m.remaining_m);

    printf("== drag takes over from follow\n");
    value_changed = 0;
    map_view_set_follow(map, true); value_changed = 0;
    double c0lat, c0lon, c1lat, c1lon; map_view_get_center(map, &c0lat, &c0lon);
    ptr.x = 240; ptr.y = 240; ptr_state = LV_INDEV_STATE_PRESSED; spin(60);
    for (int i = 0; i < 8; i++) { ptr.x -= 12; ptr.y -= 5; spin(30); }
    ptr_state = LV_INDEV_STATE_RELEASED; spin(40);
    map_view_get_center(map, &c1lat, &c1lon);
    CHECK(!map_view_get_follow(map) && value_changed >= 1, "follow off after drag, VALUE_CHANGED x%d", value_changed);
    CHECK(c1lon > c0lon && c1lat < c0lat, "finger up-left => centre moves east+south (dlon=%+.6f dlat=%+.6f)", c1lon - c0lon, c1lat - c0lat);
    spin(600);   /* let the fling settle */
    dump("03_after_drag");

    printf("== zoom + screen<->latlon round trip\n");
    map_view_set_zoom(map, 15); CHECK(map_view_get_zoom(map) == 15, "zoom 15");
    map_view_set_zoom(map, 3);  CHECK(map_view_get_zoom(map) == 15, "zoom clamped to card range -> %d", map_view_get_zoom(map));
    map_view_set_center(map, p30.lat, p30.lon); spin(700); dump("04_z15");
    double la, lo; int32_t sx, sy;
    map_view_screen_to_latlon(map, 100, 150, &la, &lo);
    bool vis = map_view_latlon_to_screen(map, la, lo, &sx, &sy);
    CHECK(vis && sx == 100 && sy == 150, "round trip (100,150) -> %.5f,%.5f -> (%d,%d)", la, lo, sx, sy);
    map_view_zoom_in(map); map_view_zoom_in(map); map_view_zoom_in(map);
    CHECK(map_view_get_zoom(map) == 17, "zoom_in clamps at 17");

    printf("== int16 safety: 15 km segment at z19 must not wrap\n");
    map_route_handle_t far = map_route_load("/tmp/maptest/routes/far.bin", false);
    CHECK(far != NULL, "far route loaded");
    map_view_set_follow(map, false);
    map_view_set_zoom_range(map, 0, 22);
    map_view_set_position(map, 0, 0, 0, false);
    map_view_set_return_target(map, 0, 0, false);
    map_view_set_route(map, far);
    map_view_set_zoom(map, 19);
    double flat, flon; map_route_get_start(far, &flat, &flon);
    map_view_set_center(map, flat, flon + 7500 / (111320.0 * cos(flat * M_PI / 180)));
    spin(300); dump("05_far_segment");
    int on_line = 0;  for (int x = 0; x < W; x++) if (near_color(fb[(H/2) * W + x], 0x2F7BEF)) on_line++;
    int off_line = 0; for (int y = 0; y < H; y++) for (int x = 0; x < W; x += 7) if (abs(y - H/2) > 14 && near_color(fb[y * W + x], 0x2F7BEF)) off_line++;
    CHECK(on_line > W * 0.9 && off_line == 0, "straight line across the screen (%d/%d px), no wrapped garbage (%d stray)", on_line, W, off_line);
    map_view_set_route(map, route); map_route_free(far);

    printf("== legacy fixed-grid API\n");
    map_tiles_config_t cfg; memset(&cfg, 0, sizeof cfg);
    cfg.base_path = "/tmp/maptest"; cfg.tile_folders[0] = "tiles1"; cfg.tile_type_count = 1;
    cfg.default_zoom = 16; cfg.default_tile_type = 0; cfg.grid_cols = 2; cfg.grid_rows = 2; cfg.use_spiram = false;
    map_tiles_handle_t h = map_tiles_init(&cfg);
    CHECK(h != NULL, "init 2x2");
    map_tiles_set_center_from_gps(h, 37.7749, -122.4194);
    int bx, by; map_tiles_get_position(h, &bx, &by);
    CHECK(bx == 10481 && by == 25330, "grid origin %d,%d (centre tile 10482,25331)", bx, by);
    int loaded = 0; for (int i = 0; i < 4; i++) loaded += map_tiles_load_tile(h, i, bx + i % 2, by + i / 2);
    CHECK(loaded == 4, "loaded %d/4 tiles", loaded);
    lv_img_dsc_t *dsc = map_tiles_get_image(h, 0);
    CHECK(dsc && dsc->header.w == 256 && dsc->header.h == 256 && dsc->header.cf == LV_IMG_CF_TRUE_COLOR && dsc->data_size == MAP_TILE_RAM_BYTES, "descriptor %ux%u cf=%u size=%u", dsc->header.w, dsc->header.h, dsc->header.cf, dsc->data_size);
    /* compare against file bytes */
    { char p[200]; snprintf(p, sizeof p, "/tmp/maptest/tiles1/16/%d/%d.bin", bx, by); FILE *f = fopen(p, "rb"); static uint8_t raw[131084]; size_t n = fread(raw, 1, sizeof raw, f); fclose(f);
#if LV_COLOR_DEPTH == 16 && !LV_COLOR_16_SWAP
      CHECK(n == 131084 && memcmp(raw + 12, dsc->data, 131072) == 0, "pixel data identical to file payload");
#else
      CHECK(n == 131084, "file read");
#endif
    }
    CHECK(!map_tiles_load_tile(h, 0, 1, 1), "missing tile reports false");
    lv_obj_t *img_scr = lv_obj_create(NULL); lv_obj_clear_flag(img_scr, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) { lv_obj_t *im = lv_img_create(img_scr); lv_img_set_src(im, map_tiles_get_image(h, i)); lv_obj_set_pos(im, (i % 2) * 240, (i / 2) * 240 - 16); }
    lv_scr_load(img_scr); spin(300); dump("06_legacy_grid");
    map_tiles_cleanup(h);

    printf("== custom file hooks + truncated tile\n");
    {
        FILE *t = fopen("/tmp/maptest/short.bin", "wb"); uint8_t junk[12 + 1000]; memset(junk, 0xAB, sizeof junk); fwrite(junk, 1, sizeof junk, t); fclose(t);
        static uint8_t ram[MAP_TILE_RAM_BYTES]; memset(ram, 0x55, sizeof ram);
        CHECK(map_tile_load_file("/tmp/maptest/short.bin", ram) == MAP_TILE_SHORT, "truncated tile reported SHORT");
        CHECK(ram[MAP_TILE_RAM_BYTES - 1] == 0 && ram[MAP_TILE_RAM_BYTES / 2] == 0, "tail blanked, not garbage");
        CHECK(map_tile_load_file("/tmp/maptest/nope.bin", ram) == MAP_TILE_MISSING, "missing tile reported MISSING");
        map_tiles_fs_t fs = { hk_open, hk_read, hk_seek, hk_close, NULL, NULL };
        map_tiles_set_fs(&fs);
        CHECK(map_tile_load_file("/tmp/maptest/tiles1/16/10482/25331.bin", ram) == MAP_TILE_OK && hook_opens == 1, "custom hook used (opens=%d)", hook_opens);
        CHECK(map_tile_cache_probe_zooms("/tmp", "x", NULL, NULL) == 0, "probe without list_dir hook fails cleanly");
        map_tiles_set_fs(NULL);
        CHECK(map_tile_load_file("/tmp/maptest/tiles1/16/10482/25331.bin", ram) == MAP_TILE_OK && hook_opens == 1, "stdio restored");
    }

    printf("== teardown\n");
    lv_scr_load(scr);
    lv_obj_del(map);          /* destroys the cache + loader task */
    map_route_free(route);
    spin(100);
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
