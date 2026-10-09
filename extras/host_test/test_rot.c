#include "harness.h"

static lv_obj_t *map;
static map_route_handle_t route;

/* Alignment: the grey road baked into the tiles is rotated by LVGL's image transform; the
 * library predicts where it should be with its own world->screen maths. Sample the road
 * across its width at many points and report how far the real centre is from the prediction. */
static void alignment(double deg, bool smooth, double *max_err, double *mean_err, int *samples)
{
    map_view_set_smooth_rotation(map, smooth);
    map_view_set_rotation(map, deg);
    spin(250);
    int n = map_route_get_point_count(route);
    double sum = 0; *max_err = 0; *samples = 0;
    for (int i = 4; i < n - 4; i++) {
        if (i >= 26 && i <= 30) continue;            /* corners: no single road direction */
        if (i >= 46 && i <= 50) continue;
        if (i >= 70 && i <= 74) continue;
        map_route_point_t a, b, c; map_route_get_point(route, i - 2, &a); map_route_get_point(route, i, &b); map_route_get_point(route, i + 2, &c);
        int32_t ax, ay, bx, by, cx, cy;
        map_view_latlon_to_screen(map, a.lat, a.lon, &ax, &ay);
        bool vis = map_view_latlon_to_screen(map, b.lat, b.lon, &bx, &by);
        map_view_latlon_to_screen(map, c.lat, c.lon, &cx, &cy);
        if (!vis || bx < 30 || bx > W - 30 || by < 30 || by > H - 30) continue;
        double dx = cx - ax, dy = cy - ay, len = hypot(dx, dy);
        if (len < 20) continue;
        double nx = -dy / len, ny = dx / len, acc = 0; int cnt = 0;
        for (int t = -14; t <= 14; t++) {
            int x = (int)lround(bx + t * nx), y = (int)lround(by + t * ny);
            if (near_color(fb[y * W + x], 0x787878)) { acc += t; cnt++; }
        }
        if (cnt < 5) continue;
        double err = fabs(acc / cnt);
        sum += err; if (err > *max_err) *max_err = err; (*samples)++;
    }
    *mean_err = *samples ? sum / *samples : 0;
}

int main(void)
{
    lv_obj_t *scr = harness_init();
    route = map_route_load("/tmp/maptest/routes/route.bin", false);
    map = map_view_create(scr);
    lv_obj_set_size(map, W, H);
    map_view_set_tile_source(map, "/tmp/maptest", "tiles1", 40, false);
    map_view_set_zoom_range(map, 15, 17);
    map_view_set_zoom(map, 17);
    map_route_point_t p40; map_route_get_point(route, 40, &p40);
    map_view_set_center(map, p40.lat, p40.lon);

    printf("== API basics\n");
    CHECK(map_view_get_rotation(map) == 0.0 && !map_view_get_heading_up(map), "default is north-up");
    map_view_set_rotation(map, -90); CHECK(map_view_get_rotation(map) == 270.0, "-90 normalises to %.1f", map_view_get_rotation(map));
    map_view_set_rotation(map, 725.04); CHECK(fabs(map_view_get_rotation(map) - 5.0) < 1e-9, "725.04 -> %.1f (0.1 deg resolution)", map_view_get_rotation(map));

    printf("== tiles stay glued to the vector layer (road under predicted position)\n");
    map_view_set_rotation(map, 0); spin(1200);        /* load the tiles once */
    /* The probe itself has a constant bias (PIL rasterisation vs LVGL pixel centres), visible at 0 degrees,
     * where the code path is the old, unchanged one. What rotation must not do is make it worse. */
    double mx, me, base_mx, base_me; int ns;
    alignment(0, false, &base_mx, &base_me, &ns);
    printf("  baseline at 0 deg (unrotated path): mean %.2f px, max %.2f px  <- probe bias, not rotation\n", base_me, base_mx);
    const double angles[] = { 20, 30, 45, 90, 137, 200, 270, 315 };
    for (unsigned k = 0; k < sizeof angles / sizeof *angles; k++) {
        alignment(angles[k], false, &mx, &me, &ns);
        CHECK(ns >= 6 && fabs(me - base_me) <= 0.5 && mx - base_mx <= 1.25, "%3.0f deg  hard edges: %2d samples, mean %+.2f px, max %+.2f px vs baseline", angles[k], ns, me - base_me, mx - base_mx);
    }
    alignment(45, true, &mx, &me, &ns);
    CHECK(ns >= 6 && fabs(me - base_me) <= 0.5 && mx - base_mx <= 1.25, " 45 deg  smooth    : %2d samples, mean %+.2f px, max %+.2f px vs baseline", ns, me - base_me, mx - base_mx);
    map_view_set_smooth_rotation(map, false);
    map_view_set_rotation(map, 30); spin(300); dump("r30_tiles");

    printf("== seams between rotated tiles (flat-colour tiles, anything not flat is a seam)\n");
    lv_obj_t *flat = map_view_create(scr);
    lv_obj_set_size(flat, W, H);
    map_view_set_tile_source(flat, "/tmp/maptest", "tiles_flat", 40, false);
    map_view_set_zoom_range(flat, 17, 17); map_view_set_zoom(flat, 17);
    map_view_set_center(flat, 37.7749, -122.4194);
    lv_obj_move_foreground(flat); lv_obj_add_flag(map, LV_OBJ_FLAG_HIDDEN);
    spin(1000);
    const double seam_angles[] = { 0, 17, 33, 45, 62, 90, 123 };
    for (int smooth = 0; smooth < 2; smooth++) {
        map_view_set_smooth_rotation(flat, smooth);
        for (unsigned k = 0; k < sizeof seam_angles / sizeof *seam_angles; k++) {
            map_view_set_rotation(flat, seam_angles[k]); spin(250);
            uint16_t ref = fb[(H / 2) * W + W / 2]; int bad = 0;
            for (int i = 0; i < W * H; i++) {
                int dr = abs((int)((fb[i] >> 11) & 31) - (int)((ref >> 11) & 31)), dg = abs((int)((fb[i] >> 5) & 63) - (int)((ref >> 5) & 63)), db = abs((int)(fb[i] & 31) - (int)(ref & 31));
                if (dr > 1 || dg > 2 || db > 1) bad++;
            }
            if (bad > 0 && bad <= 6) { int shown = 0; for (int i = 0; i < W * H && shown < 6; i++) { int dr = abs((int)((fb[i] >> 11) & 31) - (int)((ref >> 11) & 31)); int dg = abs((int)((fb[i] >> 5) & 63) - (int)((ref >> 5) & 63)); int db = abs((int)(fb[i] & 31) - (int)(ref & 31)); if (dr > 1 || dg > 2 || db > 1) { printf("        stray px at (%d,%d)\n", i % W, i / W); shown++; } } }
            if (seam_angles[k] == 33) { char nm[40]; snprintf(nm, sizeof nm, "seam_33_%s", smooth ? "smooth" : "hard"); dump(nm); }
            printf("  %s  %3.0f deg  %6d / %d off-colour px\n", smooth ? "smooth" : "hard  ", seam_angles[k], bad, W * H);
            CHECK(bad <= 8, "%s, %.0f deg: no seam lines (%d stray px)", smooth ? "smooth" : "hard", seam_angles[k], bad);
        }
    }
    map_view_set_rotation(flat, 33); map_view_set_smooth_rotation(flat, false);
    lv_obj_del(flat); lv_obj_clear_flag(map, LV_OBJ_FLAG_HIDDEN);

    printf("== screen <-> lat/lon under rotation\n");
    for (double deg = 0; deg < 360; deg += 61) {
        map_view_set_rotation(map, deg); double la, lo; int32_t x, y, x2, y2;
        map_view_screen_to_latlon(map, 100, 150, &la, &lo);
        bool v1 = map_view_latlon_to_screen(map, la, lo, &x, &y);
        map_view_screen_to_latlon(map, 410, 77, &la, &lo);
        bool v2 = map_view_latlon_to_screen(map, la, lo, &x2, &y2);
        CHECK(v1 && v2 && x == 100 && y == 150 && x2 == 410 && y2 == 77, "%3.0f deg round trip: (100,150)->(%d,%d)  (410,77)->(%d,%d)", deg, x, y, x2, y2);
    }
    { /* the view centre must be the same place whatever the rotation */
        double la0, lo0, la1, lo1;
        map_view_set_rotation(map, 0);   map_view_screen_to_latlon(map, W / 2, H / 2, &la0, &lo0);
        map_view_set_rotation(map, 123); map_view_screen_to_latlon(map, W / 2, H / 2, &la1, &lo1);
        CHECK(fabs(la0 - la1) < 1e-9 && fabs(lo0 - lo1) < 1e-9, "screen centre is the rotation pivot");
        map_view_set_rotation(map, 90);  int32_t x, y; map_view_set_zoom(map, 17);
        double clat, clon; map_view_get_center(map, &clat, &clon);
        map_view_latlon_to_screen(map, clat + 100 / 111320.0, clon, &x, &y);   /* 100 m north of centre, east is up */
        double mpp = 156543.03392 * cos(clat * M_PI / 180) / (1 << 17);        /* metres per pixel at z17 */
        int want = (int)lround(100 / mpp);
        CHECK(abs((W / 2 - x) - want) <= 2 && abs(y - H / 2) <= 2, "at 90 deg, 100 m north (= %d px) lies to the LEFT of centre: (%d,%d)", want, x, y);
    }

    printf("== dragging moves the map with the finger at any rotation\n");
    const double drag_angles[] = { 0, 33, 90, 215 };
    for (unsigned k = 0; k < sizeof drag_angles / sizeof *drag_angles; k++) {
        map_view_set_follow(map, false); map_view_set_rotation(map, drag_angles[k]);
        map_view_set_center(map, p40.lat, p40.lon); spin(50);
        double la0, lo0; map_view_screen_to_latlon(map, 300, 300, &la0, &lo0);
        ptr.x = 300; ptr.y = 300; ptr_state = LV_INDEV_STATE_PRESSED; spin(60);
        for (int i = 0; i < 10; i++) { ptr.x -= 9; ptr.y += 4; spin(30); }
        ptr_state = LV_INDEV_STATE_RELEASED; spin(30);
        int32_t x, y; map_view_latlon_to_screen(map, la0, lo0, &x, &y);
        double vx = x - 300, vy = y - 300;
        double ang = fabs(atan2(vy, vx) - atan2(40.0, -90.0)) * 180 / M_PI; if (ang > 180) ang = 360 - ang;
        CHECK(hypot(vx, vy) > 60 && ang < 3.0, "%3.0f deg: point under the finger moved (%+.0f,%+.0f), %.1f deg off the finger's path", drag_angles[k], vx, vy, ang);
        spin(500);
    }

    printf("== heading-up\n");
    map_view_set_rotation(map, 0); map_view_set_zoom(map, 17);
    map_view_set_position(map, p40.lat, p40.lon, 90, true);
    map_view_set_follow(map, true); map_view_set_follow_offset(map, 80);
    map_view_set_heading_up(map, true);
    CHECK(map_view_get_heading_up(map), "enabled");
    spin(30); double r_early = map_view_get_rotation(map);
    CHECK(r_early > 0.5 && r_early < 89, "eases rather than snaps (%.1f deg after 30 ms)", r_early);
    spin(1500);
    CHECK(fabs(map_view_get_rotation(map) - 90) < 0.3, "settles on the heading: %.1f deg", map_view_get_rotation(map));
    { int32_t mx_, my_; map_view_latlon_to_screen(map, p40.lat, p40.lon, &mx_, &my_);
      CHECK(abs(mx_ - W / 2) <= 2 && abs(my_ - (H / 2 + 80)) <= 2, "marker sits %d px below centre, in the middle (%d,%d)", 80, mx_, my_);
      /* arrow points up on screen: pixels above the dot are marker blue, pixels below it are not */
      int r = 12; bool up = near_color(fb[(my_ - r - 8) * W + mx_], 0x1A73E8), down = near_color(fb[(my_ + r + 8) * W + mx_], 0x1A73E8);
      CHECK(up && !down, "heading arrow points up the screen"); dump("r90_headingup"); }
    map_view_set_position(map, p40.lat, p40.lon, 350, true);        /* 90 -> 350 is a 100 degree turn LEFT, not 260 right */
    spin(60); double r_mid = map_view_get_rotation(map);
    CHECK(r_mid < 90 && r_mid > 20, "takes the short way round (%.1f deg, falling)", r_mid);
    spin(1500);
    CHECK(fabs(map_view_get_rotation(map) - 350) < 0.3, "settles on 350: %.1f", map_view_get_rotation(map));
    map_view_set_position(map, p40.lat, p40.lon, 10, true); spin(1500);
    CHECK(fabs(map_view_get_rotation(map) - 10) < 0.3, "350 -> 10 crosses north the short way: %.1f", map_view_get_rotation(map));
    /* panning away freezes rotation */
    map_view_set_follow(map, false);
    map_view_set_position(map, p40.lat, p40.lon, 200, true); spin(500);
    CHECK(fabs(map_view_get_rotation(map) - 10) < 0.3, "no spinning while follow is off: %.1f", map_view_get_rotation(map));
    map_view_set_follow(map, true); spin(2000);
    CHECK(fabs(map_view_get_rotation(map) - 200) < 0.3, "resumes when follow is back: %.1f", map_view_get_rotation(map));
    map_view_set_heading_up(map, false); spin(2500);
    CHECK(fabs(map_view_get_rotation(map)) < 0.3 || fabs(map_view_get_rotation(map) - 360) < 0.3, "heading-up off eases back to north: %.1f", map_view_get_rotation(map));
    map_view_set_heading_up(map, true); map_view_set_rotation(map, 45);
    CHECK(!map_view_get_heading_up(map) && map_view_get_rotation(map) == 45.0, "manual set_rotation turns heading-up off and sticks");

    printf("== route + marker + tether stay on the road while rotated\n");
    map_view_set_route(map, route); map_view_set_follow(map, false); map_view_set_zoom(map, 17);
    map_view_set_return_target(map, p40.lat, p40.lon - 0.001, false);
    map_view_set_position(map, p40.lat, p40.lon, 0, true);
    map_view_set_center(map, p40.lat, p40.lon);
    for (unsigned k = 0; k < 4; k++) {
        double deg = 25 + 77 * k; map_view_set_rotation(map, deg); spin(300);
        int blue = 0, casing = 0, grey_leak = 0;
        for (int y = 3; y < H - 3; y++) for (int x = 3; x < W - 3; x++) {
            if (near_color(fb[y * W + x], 0x2F7BEF)) { blue++; for (int dy = -3; dy <= 3; dy++) for (int dx = -3; dx <= 3; dx++) if (near_color(fb[(y + dy) * W + x + dx], 0x787878)) grey_leak++; }
            if (near_color(fb[y * W + x], 0x123A6E)) casing++;
        }
        CHECK(blue > 400 && casing > 300 && grey_leak == 0, "%3.0f deg: route drawn (core %d, casing %d), road fully covered (leak %d)", deg, blue, casing, grey_leak);
        if (k == 1) dump("r102_route");
    }

    printf("== frame cost (host CPU, full-screen redraw through 10 draw chunks; ratios matter, not ms)\n");
    map_view_set_position(map, 0, 0, 0, false); map_view_set_route(map, NULL);
    struct { const char *name; double deg; bool smooth; } cases[] = {
        {"north-up (plain blit)", 0, false}, {"rotated 20 deg, hard", 20, false}, {"rotated 45 deg, hard", 45, false},
        {"rotated 45 deg, smooth", 45, true}, {"rotated 90 deg, hard", 90, false},
    };
    double base = 0;
    for (unsigned k = 0; k < sizeof cases / sizeof *cases; k++) {
        map_view_set_smooth_rotation(map, cases[k].smooth); map_view_set_rotation(map, cases[k].deg); spin(700);
        for (int i = 0; i < 3; i++) { lv_obj_invalidate(map); lv_refr_now(NULL); }
        double t0 = now_ms(); const int N = 40;
        for (int i = 0; i < N; i++) { lv_obj_invalidate(map); lv_refr_now(NULL); }
        double ms = (now_ms() - t0) / N; if (k == 0) base = ms;
        printf("  %-26s %7.2f ms/frame   x%.1f\n", cases[k].name, ms, ms / base);
    }

    lv_obj_del(map); map_route_free(route); spin(100);
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
