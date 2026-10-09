/*
 * TileGrid - the original fixed-grid API (map_tiles_* functions)
 *
 * Loads a 3x3 block of 256x256 tiles around a GPS position from the SD card, puts
 * them in a container of lv_img widgets and slides the container so the position
 * sits in the middle of the screen, with a red dot on top.
 *
 * Use this when you just want "a map picture". For a smooth, draggable, route-aware
 * map use the map_view_* API instead (see the MapViewFollow example).
 *
 * Needs: LVGL 8.3.x with LV_COLOR_DEPTH 16 (or 32), an SD card with tiles in
 *        /sd/tiles1/<zoom>/<x>/<y>.bin  (make them with scripts/lvgl_map_tile_converter.py)
 */
#include <Arduino.h>
#include <SD.h>
#include <lvgl.h>
#include <MapTiles.h>

#define SD_CS        5               // chip-select of your SD slot
#define TILE_BASE    "/sd"           // Arduino SD mounts at /sd; SD_MMC = "/sdcard", LittleFS = "/littlefs"
#define TILE_FOLDER  "tiles1"
#define ZOOM         16
#define GRID         3               // 3x3 tiles = 9 * 128 KB = 1.1 MB (use PSRAM!)

static const double LAT = 37.7749, LON = -122.4194;     // <-- put a place you have tiles for

static map_tiles_handle_t g_map;
static lv_obj_t *g_cont;
static lv_obj_t *g_img[GRID * GRID];
static lv_obj_t *g_marker;

/* ---------------------------------------------------------------------------
 * 1) YOUR DISPLAY (+ touch) SETUP GOES HERE.
 *    Anything that registers a normal LVGL 8 display works: TFT_eSPI, LovyanGFX,
 *    Arduino_GFX, esp_lcd... Call lv_init() first, then lv_disp_drv_register().
 * ------------------------------------------------------------------------- */
static void setupDisplay()
{
    lv_init();
    // ... lv_disp_draw_buf_init(), lv_disp_drv_init(), lv_disp_drv_register() ...
}

static void showLocation(double lat, double lon)
{
    map_tiles_set_center_from_gps(g_map, lat, lon);

    int base_x, base_y;
    map_tiles_get_position(g_map, &base_x, &base_y);          // top-left tile of the grid

    for (int i = 0; i < GRID * GRID; i++) {
        int col = i % GRID, row = i / GRID;
        if (map_tiles_load_tile(g_map, i, base_x + col, base_y + row)) {
            lv_img_set_src(g_img[i], map_tiles_get_image(g_map, i));
            lv_obj_clear_flag(g_img[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_img[i], LV_OBJ_FLAG_HIDDEN);    // tile not on the card
        }
    }

    // Where the GPS point lies inside the grid, in pixels
    double tx, ty;
    map_tiles_gps_to_tile_xy(g_map, lat, lon, &tx, &ty);
    int mx = (int)((tx - base_x) * MAP_TILES_TILE_SIZE);
    int my = (int)((ty - base_y) * MAP_TILES_TILE_SIZE);

    lv_obj_set_pos(g_marker, mx - 7, my - 7);
    lv_obj_set_pos(g_cont, lv_disp_get_hor_res(NULL) / 2 - mx, lv_disp_get_ver_res(NULL) / 2 - my);
}

void setup()
{
    Serial.begin(115200);
    setupDisplay();
    if (!lv_disp_get_default()) {
        Serial.println("No LVGL display registered - fill in setupDisplay() first.");
        return;
    }

    if (!SD.begin(SD_CS)) { Serial.println("SD mount failed"); return; }

    map_tiles_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_path         = TILE_BASE;
    cfg.tile_folders[0]   = TILE_FOLDER;
    cfg.tile_type_count   = 1;
    cfg.default_tile_type = 0;
    cfg.default_zoom      = ZOOM;
    cfg.grid_cols         = GRID;
    cfg.grid_rows         = GRID;
    cfg.use_spiram        = true;          // falls back to internal RAM if there is no PSRAM
    g_map = map_tiles_init(&cfg);
    if (!g_map) { Serial.println("map_tiles_init failed"); return; }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    g_cont = lv_obj_create(scr);
    lv_obj_set_size(g_cont, GRID * MAP_TILES_TILE_SIZE, GRID * MAP_TILES_TILE_SIZE);
    lv_obj_set_style_pad_all(g_cont, 0, 0);
    lv_obj_set_style_border_width(g_cont, 0, 0);
    lv_obj_set_style_radius(g_cont, 0, 0);
    lv_obj_clear_flag(g_cont, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < GRID * GRID; i++) {
        g_img[i] = lv_img_create(g_cont);
        lv_obj_set_pos(g_img[i], (i % GRID) * MAP_TILES_TILE_SIZE, (i / GRID) * MAP_TILES_TILE_SIZE);
    }

    g_marker = lv_obj_create(g_cont);
    lv_obj_set_size(g_marker, 14, 14);
    lv_obj_set_style_radius(g_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_marker, lv_color_hex(0xE03B24), 0);
    lv_obj_set_style_border_color(g_marker, lv_color_white(), 0);
    lv_obj_set_style_border_width(g_marker, 2, 0);
    lv_obj_clear_flag(g_marker, LV_OBJ_FLAG_SCROLLABLE);

    showLocation(LAT, LON);
}

void loop()
{
    static uint32_t last = 0;
    uint32_t now = millis();
    lv_tick_inc(now - last);            // not needed if lv_conf.h has LV_TICK_CUSTOM 1
    last = now;
    lv_timer_handler();
    delay(5);
}
