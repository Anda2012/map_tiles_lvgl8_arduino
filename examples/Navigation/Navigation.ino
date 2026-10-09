/*
 * Navigation - route.bin + map matching + turn-by-turn text, with a built-in simulator
 *
 * Loads /sd/routes/route.bin (made by the exporter described in the README), draws it
 * on a following map and turns each position fix into: distance to the next turn,
 * the instruction, distance remaining, and - when you leave the road - which way it is.
 *
 * Real GPS: replace simulatedFix() with your NMEA / u-blox / TinyGPS++ reader and call
 * onFix(lat, lon, course) from loop() (the LVGL thread - it touches widgets).
 */
#include <Arduino.h>
#include <SD.h>
#include <lvgl.h>
#include <MapTiles.h>

#define SD_CS          5
#define TILE_BASE      "/sd"
#define DEFAULT_FOLDER "tiles1"
#define ROUTE_PATH     TILE_BASE "/routes/route.bin"
#define CACHE_SLOTS    32
#define OFF_ROUTE_M    35.0
#define SIM_SPEED_MPS  15.0

static map_route_handle_t g_route;
static lv_obj_t *g_map;
static lv_obj_t *g_banner;

static void setupDisplay()               // <-- YOUR DISPLAY SETUP, see TileGrid.ino
{
    lv_init();
}

static bool startNavigation()
{
    map_route_info_t info;
    if (!map_route_peek(ROUTE_PATH, &info)) { Serial.println("route.bin missing or not an MRT1 file"); return false; }
    Serial.printf("Route '%s': %.2f km, z%u-z%u%s\n", info.name, info.distance_m / 1000.0,
                  (unsigned)info.min_zoom, (unsigned)info.max_zoom, info.loop ? ", loop" : "");

    g_route = map_route_load(ROUTE_PATH, true);
    if (!g_route) return false;

    g_map = map_view_create(lv_scr_act());
    lv_obj_set_size(g_map, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));

    const char *folder = map_route_get_tile_folder(g_route);       // the route may name its own tile set
    if (!folder || !folder[0]) folder = DEFAULT_FOLDER;
    if (!map_view_set_tile_source(g_map, TILE_BASE, folder, CACHE_SLOTS, true)) return false;

    int zmin, zmax;
    if (map_tile_cache_probe_zooms(TILE_BASE, folder, &zmin, &zmax) > 0) {
        map_view_set_zoom_range(g_map, zmin, zmax);
        map_view_set_zoom(g_map, zmax);
    }

    map_view_set_route(g_map, g_route);
    map_view_set_follow(g_map, true);
    map_view_set_follow_offset(g_map, 50);

    double lat, lon;
    if (map_route_get_start(g_route, &lat, &lon)) map_view_set_center(g_map, lat, lon);

    g_banner = lv_label_create(lv_scr_act());
    lv_obj_set_width(g_banner, lv_disp_get_hor_res(NULL));
    lv_obj_set_style_bg_color(g_banner, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_banner, LV_OPA_70, 0);
    lv_obj_set_style_text_color(g_banner, lv_color_white(), 0);
    lv_obj_set_style_pad_all(g_banner, 6, 0);
    lv_obj_align(g_banner, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(g_banner, "Waiting for a fix...");
    return true;
}

/* One position fix in, map + banner out. */
static void onFix(double lat, double lon, double course_deg)
{
    map_route_match_t m;
    if (!g_route || !map_route_match(g_route, lat, lon, OFF_ROUTE_M, &m)) return;

    map_view_set_position(g_map, lat, lon, course_deg, true);
    map_view_set_progress(g_map, m.traveled_m);
    map_view_set_off_route(g_map, !m.on_route);
    map_view_set_return_target(g_map, m.return_lat, m.return_lon, !m.on_route);

    char gap[24], rest[24], to_turn[24];
    map_geo_format_distance(m.remaining_m, false, rest, sizeof(rest));

    if (!m.on_route) {
        double rel = map_geo_angle_diff_deg(course_deg, m.return_bearing_deg);
        map_geo_format_distance(m.cross_track_m, false, gap, sizeof(gap));
        lv_label_set_text_fmt(g_banner, "Off route - road is %s %s", gap,
                              rel < -25 ? "to your left" : rel > 25 ? "to your right" : "ahead");
    } else if (m.arrived) {
        lv_label_set_text(g_banner, LV_SYMBOL_OK " Arrived");
    } else if (m.next_maneuver >= 0) {
        map_maneuver_t turn;
        map_route_get_maneuver(g_route, m.next_maneuver, &turn);
        map_geo_format_distance(m.dist_to_maneuver_m, false, to_turn, sizeof(to_turn));
        lv_label_set_text_fmt(g_banner, "%s %s in %s\n%s   (%s left)",
                              map_maneuver_type_symbol(turn.type), map_maneuver_type_name(turn.type),
                              to_turn, turn.street[0] ? turn.street : "the road ahead", rest);
    } else {
        lv_label_set_text_fmt(g_banner, LV_SYMBOL_UP " Continue   (%s left)", rest);
    }
}

/* Walk along the route at SIM_SPEED_MPS; wobble sideways after 20 s to show the off-route UI. */
static void simulatedFix(double *lat, double *lon, double *course)
{
    static uint32_t t0 = millis();
    double dist = (millis() - t0) / 1000.0 * SIM_SPEED_MPS;
    double total = map_route_get_total_distance_m(g_route);
    if (dist > total) dist = total;

    int n = map_route_get_point_count(g_route);
    map_route_point_t a, b;
    int i = 0;
    for (; i < n - 2; i++) {                       // fine for a demo; a real app would keep the index
        map_route_get_point(g_route, i + 1, &b);
        if (b.cum_dist_m >= dist) break;
    }
    map_route_get_point(g_route, i, &a);
    map_route_get_point(g_route, i + 1, &b);
    double span = b.cum_dist_m - a.cum_dist_m;
    double t = span > 0 ? (dist - a.cum_dist_m) / span : 0;
    *lat = a.lat + (b.lat - a.lat) * t;
    *lon = a.lon + (b.lon - a.lon) * t;
    *course = a.bearing_deg;

    double since = (millis() - t0) / 1000.0;
    if (since > 20 && since < 28) *lon += 80.0 / (111320.0 * cos(*lat * M_PI / 180.0));   // ~80 m east
}

void setup()
{
    Serial.begin(115200);
    setupDisplay();
    if (!lv_disp_get_default()) { Serial.println("No LVGL display registered - fill in setupDisplay()."); return; }
    if (!SD.begin(SD_CS))       { Serial.println("SD mount failed"); return; }
    startNavigation();
}

void loop()
{
    static uint32_t last = 0, last_fix = 0;
    uint32_t now = millis();
    lv_tick_inc(now - last);
    last = now;

    if (g_route && now - last_fix >= 200) {        // 5 Hz
        last_fix = now;
        double lat, lon, course;
        simulatedFix(&lat, &lon, &course);
        onFix(lat, lon, course);
    }
    lv_timer_handler();
    delay(5);
}
