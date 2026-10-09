/*
 * MapViewFollow - the smooth map widget (map_view_*) with a simulated vehicle
 *
 * One LVGL object draws the whole map: tiles come from a background loader task so
 * the UI never blocks on the SD card, the view is continuous (drag it, it flings),
 * and "follow" mode eases the map after a moving marker. A vehicle circles a point
 * so you can watch it without a GPS. Touch-drag the map to break follow; the
 * button in the corner turns it back on. +/- change zoom, the compass button
 * switches between north-up and heading-up (the map turns so the vehicle drives "up").
 *
 * Needs: LVGL 8.3.x, a display (+ touch for the buttons), PSRAM strongly recommended
 *        (each cached tile is 128 KB), tiles in /sd/tiles1/<zoom>/<x>/<y>.bin
 */
#include <Arduino.h>
#include <SD.h>
#include <lvgl.h>
#include <MapTiles.h>

#define SD_CS         5
#define TILE_BASE     "/sd"
#define TILE_FOLDER   "tiles1"
#define CACHE_SLOTS   32                 // 128 KB each. >= visible tiles + a ring around them.

static const double CENTER_LAT = 37.7749, CENTER_LON = -122.4194;   // <-- where your tiles are
static const double RADIUS_M   = 250.0;                              // simulated circle
static const double SPEED_MPS  = 12.0;

static lv_obj_t *g_map;
static lv_obj_t *g_recenter;
static lv_obj_t *g_compass_label;

static void setupDisplay()               // <-- YOUR DISPLAY (+ touch) SETUP, see TileGrid.ino
{
    lv_init();
}

static void onMapChanged(lv_event_t *e)  // follow mode toggled (also fires when the user drags)
{
    if (!g_recenter) return;                 // fires once during setup, before the button exists
    if (map_view_get_follow(g_map)) lv_obj_add_flag(g_recenter, LV_OBJ_FLAG_HIDDEN);
    else                            lv_obj_clear_flag(g_recenter, LV_OBJ_FLAG_HIDDEN);
}

static void onButton(lv_event_t *e)
{
    intptr_t what = (intptr_t)lv_event_get_user_data(e);
    if (what == 0)      map_view_zoom_in(g_map);
    else if (what == 1) map_view_zoom_out(g_map);
    else if (what == 2) map_view_set_follow(g_map, true);
    else {
        // Heading-up turns the map through LVGL's image transform: smoother to look at, but
        // several times more work per frame than north-up, so it is a switch, not the default.
        bool on = !map_view_get_heading_up(g_map);
        map_view_set_heading_up(g_map, on);
        lv_label_set_text(g_compass_label, on ? "H" : "N");
    }
}

static lv_obj_t *makeButton(lv_obj_t *parent, const char *text, lv_align_t align, int x, int y, intptr_t id)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 48, 48);
    lv_obj_align(b, align, x, y);
    lv_obj_add_event_cb(b, onButton, LV_EVENT_CLICKED, (void *)id);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

void setup()
{
    Serial.begin(115200);
    setupDisplay();
    if (!lv_disp_get_default()) { Serial.println("No LVGL display registered - fill in setupDisplay()."); return; }
    if (!SD.begin(SD_CS))       { Serial.println("SD mount failed"); return; }

    g_map = map_view_create(lv_scr_act());
    lv_obj_set_size(g_map, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));
    lv_obj_add_event_cb(g_map, onMapChanged, LV_EVENT_VALUE_CHANGED, NULL);

    if (!map_view_set_tile_source(g_map, TILE_BASE, TILE_FOLDER, CACHE_SLOTS, true)) {
        Serial.println("tile cache failed (out of memory?)");
        return;
    }

    // Only allow zoom levels that are really on the card, otherwise you get blank tiles.
    int zmin, zmax;
    if (map_tile_cache_probe_zooms(TILE_BASE, TILE_FOLDER, &zmin, &zmax) > 0) {
        map_view_set_zoom_range(g_map, zmin, zmax);
        map_view_set_zoom(g_map, zmax);
    }

    map_view_set_center(g_map, CENTER_LAT, CENTER_LON);
    map_view_set_follow(g_map, true);
    map_view_set_follow_offset(g_map, 60);     // keep the marker a bit below centre: more road ahead

    makeButton(g_map, "+", LV_ALIGN_TOP_RIGHT, -8, 8, 0);
    makeButton(g_map, "-", LV_ALIGN_TOP_RIGHT, -8, 64, 1);
    g_recenter = makeButton(g_map, LV_SYMBOL_GPS, LV_ALIGN_BOTTOM_RIGHT, -8, -8, 2);
    lv_obj_add_flag(g_recenter, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *compass = makeButton(g_map, "N", LV_ALIGN_BOTTOM_LEFT, 8, -8, 3);
    g_compass_label = lv_obj_get_child(compass, 0);
}

static void simulateVehicle()
{
    static uint32_t t0 = millis();
    double s = (millis() - t0) / 1000.0 * SPEED_MPS;            // metres travelled
    double a = s / RADIUS_M;                                    // angle on the circle (radians)

    double dn =  RADIUS_M * sin(a);                             // metres north of centre
    double de =  RADIUS_M * (1.0 - cos(a)) - RADIUS_M;          // metres east of centre
    double lat = CENTER_LAT + dn / 111320.0;
    double lon = CENTER_LON + de / (111320.0 * cos(CENTER_LAT * M_PI / 180.0));
    double heading = fmod(a * 180.0 / M_PI, 360.0);              // travelling clockwise: course == angle

    map_view_set_position(g_map, lat, lon, heading, true);
}

void loop()
{
    static uint32_t last = 0, last_sim = 0;
    uint32_t now = millis();
    lv_tick_inc(now - last);
    last = now;

    if (g_map && now - last_sim >= 100) {       // a GPS-like 10 Hz update
        last_sim = now;
        simulateVehicle();
    }
    lv_timer_handler();
    delay(5);
}
