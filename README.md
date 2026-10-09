# map_tiles_lvgl8

Offline map tiles for **LVGL 8.x** on **Arduino / PlatformIO** (ESP32).

This is a port of [0015/map_tiles](https://github.com/0015/map_tiles) (v2.0.0, an ESP-IDF component for
LVGL 9) to LVGL 8.3 and the Arduino build system. The public API is the same; the tile files, the converter
script and the `route.bin` format are unchanged, so existing SD cards keep working. MIT licensed, original
copyright Eric Nam - see `LICENSE`.

What you get:

* **`map_view_*`** - one LVGL widget that draws the whole map: continuous (drag + fling) coordinates, tiles
  read on a background task so the UI never waits on the SD card, route polyline, position marker with
  heading, follow mode, off-route tether, and **map rotation / heading-up**.
* **`map_route_*`** - load `route.bin`, snap GPS fixes onto it (on/off route, distance remaining, next turn,
  arrival, laps).
* **`map_tiles_*`** - the original fixed-grid API (load N x N tiles into `lv_img` widgets).
* **`map_geo_*`** - Web-Mercator / great-circle helpers.

> **Status:** verified on a PC against real LVGL 8.3.11 (`extras/host_test`: rendering, route alignment,
> matching, rotation, int16 safety, all three colour setups, ASan/UBSan). **Not yet run on a physical board** - SD/SPI
> timing, PSRAM, your display driver and (for rotation) the real frame rate are the things to watch the first time.

## Install

**PlatformIO**

```ini
[env:esp32s3]
platform  = espressif32
board     = esp32-s3-devkitc-1
framework = arduino
board_build.arduino.memory_type = qio_opi      ; whatever matches your board's PSRAM
build_flags =
    -DBOARD_HAS_PSRAM
    -DLV_CONF_INCLUDE_SIMPLE
    -Iinclude                                   ; folder that holds your lv_conf.h
lib_deps =
    lvgl/lvgl@^8.3.11
    map_tiles_lvgl8                             ; or a path / git URL to this folder
```

**Arduino IDE** - copy this folder to `Arduino/libraries/`, install **lvgl 8.3.x** (not 9.x) from the Library
Manager, enable PSRAM in *Tools* if your board has it.

## lv_conf.h requirements

| Setting | Needed |
|---|---|
| `LV_COLOR_DEPTH` | **16** (normal) or **32**. 32 works but each tile costs 256 KB instead of 128 KB. |
| `LV_COLOR_16_SWAP` | Either. Handled when tiles are loaded (they are byte-swapped for you). |
| `LV_USE_IMG` | 1 (default) |
| `LV_MEM_SIZE` | The tiles are **not** in LVGL's heap, so the default is fine. |

A wrong LVGL major version or colour depth stops the build with an explanatory `#error`.

## Quick start

```cpp
#include <SD.h>
#include <lvgl.h>
#include <MapTiles.h>

lv_obj_t *map;

void setup() {
  // ... lv_init() + your display driver ...
  SD.begin(5);

  map = map_view_create(lv_scr_act());
  lv_obj_set_size(map, 480, 320);
  map_view_set_tile_source(map, "/sd", "tiles1", 32 /*slots, 128 KB each*/, true /*PSRAM*/);

  int zmin, zmax;                                   // clamp to what is really on the card
  if (map_tile_cache_probe_zooms("/sd", "tiles1", &zmin, &zmax) > 0) {
    map_view_set_zoom_range(map, zmin, zmax);
    map_view_set_zoom(map, zmax);
  }
  map_view_set_center(map, 37.7749, -122.4194);
}

// on each GPS fix, from the LVGL thread:
//   map_view_set_position(map, lat, lon, heading_deg, true);
```

Three complete sketches are in `examples/` (each has a marked spot for your display driver):
`TileGrid` (fixed-grid API), `MapViewFollow` (smooth view + simulated vehicle), `Navigation`
(route + matching + turn banner, with a built-in simulator).

## Rotation and heading-up

```cpp
map_view_set_rotation(map, 90);       // east points up. 0 = north up (default)
map_view_set_heading_up(map, true);   // the map eases round so the vehicle's heading points up
map_view_set_heading_up(map, false);  // ...and eases back to north
```

Rotation is the *compass bearing that points up on screen*. Route, marker, off-route tether, dragging, the follow
offset, `map_view_screen_to_latlon` / `latlon_to_screen` and `fit_bounds` all follow it. Heading-up only turns the
map while follow is on, so dragging the map away does not make it spin under your finger. A manual
`set_rotation` switches heading-up off, so the angle you set is the angle you keep. GPS course is noisy at low
speed - hold the heading you pass to `map_view_set_position` while stopped.

What it costs (measured on the PC host test, 480x480, 10 draw chunks per frame; **ratios, not ESP32 times**):

| Mode | Cost vs. north-up |
|---|---|
| north-up (plain copy of each tile) | 1x |
| rotated, hard edges (default) | about 2.5-4x |
| rotated, `map_view_set_smooth_rotation(map, true)` | about 7.5-13x |

* Rotated tiles go through LVGL's software image transform instead of a copy, and that happens on **every** frame
  while you drive, because the map moves each frame. Try it on your board at your resolution before committing to it;
  if it is too slow, use north-up (the marker's arrow still turns) or rotate in coarse steps with `set_rotation`.
* Smooth mode draws every tile twice (hard pass, then bilinear on top) so the borders stay seamless. Hard edges look
  jagged on text and thin lines at oblique angles; smooth is better for a parked, looking-at-the-map screen.
* Memory: a turned screen needs a few more tiles than a straight one - about 1-2 more on average and up to 3 more at
  worst (480x480: 9 -> 12). The pre-loading ring around the screen needs roughly 36 slots rotated instead of 25.
* Needs `LV_DRAW_COMPLEX 1` (the default); with 0 the build warns and the map stays north-up.
* Rotated tiles are checked against the vector layer to within about a pixel, and have no seams (at most an isolated
  stray pixel where four tiles meet). At 0 degrees the output is pixel-identical to the north-up-only version.

## Tiles on the card

Layout `<base>/<folder>/<zoom>/<x>/<y>.bin`, each file = 12-byte LVGL header + 256x256 RGB565 (little endian).
Make them from XYZ PNG tiles with `scripts/lvgl_map_tile_converter.py` (needs Pillow; see `scripts/README.md`).

`base_path` is a **VFS mount point** because file access defaults to stdio:

| Card / FS | `base_path` |
|---|---|
| `SD` (SPI) | `"/sd"` |
| `SD_MMC` | `"/sdcard"` |
| `LittleFS` | `"/littlefs"` |
| `SPIFFS` | `"/spiffs"` |

Prefer passing the FS object, or not on ESP32 at all? Include `MapTilesArduinoFS.h` and call
`map_tiles_use_arduino_fs(SD)` (paths are then relative to the FS root, e.g. base `""`), or install your own
hooks with `map_tiles_set_fs()` (SdFat, other cores). Hooks run on the loader task as well as the LVGL
thread, so they must be thread-safe. LVGL's own `lv_fs` is deliberately not used for this: its drivers allocate
from LVGL's heap, which must not be touched from a second task.

## Things worth knowing

* **Threading.** Call every `map_view_*` / `lv_*` function from the one thread that runs `lv_timer_handler()`
  (as always with LVGL). The library starts one FreeRTOS task (`map_tile_ld`, priority 3, 6 KB stack) that only
  reads files.
* **SPI sharing.** If the display and the SD card share an SPI bus, the loader task and your flush callback can
  collide. Use separate buses, or guard both with one mutex.
* **Memory.** One cached tile = 128 KB. A 480x320 view needs 6-9 on screen; `map_view` also pre-loads the ring
  around them when the cache is big enough (about 25+ slots for that size). Use PSRAM. With
  `use_spiram = true` and no PSRAM the buffers silently come from internal RAM and will run out quickly.
* **Logging.** Warnings and errors go to `Serial`. `-DMAP_TILES_LOG_LEVEL=3` adds info, `4` debug, `0` silences;
  `map_tiles_set_log_handler()` redirects it. A missing zoom level shows up as the first few
  "Tile not on card" lines.
* **Rotation and LVGL's draw unit.** If you use a hardware draw unit (e.g. an accelerator patched into
  `draw_ctx`), it must support image rotation or LVGL falls back to software for those calls.
* **Image cache.** `lv_img_dsc_t` descriptors point into the library's buffers; after `map_tiles_load_tile()`
  re-set the source (`lv_img_set_src`) so LVGL redraws.

## Moving code from the LVGL 9 version

The `map_*` API is identical. What changes is the LVGL side of your own code:

| LVGL 9 | LVGL 8 |
|---|---|
| `lv_image_dsc_t`, `lv_image_create`, `lv_image_set_src` | `lv_img_dsc_t`, `lv_img_create`, `lv_img_set_src` |
| `lv_obj_remove_flag` / `lv_obj_delete` | `lv_obj_clear_flag` / `lv_obj_del` |
| `lv_obj_send_event` | `lv_event_send` |
| `lv_screen_active()` | `lv_scr_act()` |
| `.tile_folders = {...}` designated init | assign fields (C++ has no array designators); see `TileGrid.ino` |
| ESP-IDF: `idf.py add-dependency`, `ESP_LOGx`, `heap_caps_*` | PlatformIO / Arduino `lib_deps`, `MAP_TILES_LOG_LEVEL`, handled inside |

## What was changed in the port (beyond renames)

* `map_view` rewritten for LVGL 8's `lv_draw_ctx_t` pipeline (`lv_draw_img/rect/line/polygon/label`).
* **New in this port:** map rotation and heading-up (`map_view_set_rotation` / `_set_heading_up` / `_set_smooth_rotation`).
  The upstream LVGL 9 component is north-up only.
* **int16 coordinate safety.** LVGL 8's `lv_coord_t` is 16-bit; a route segment that runs 100 000 px off screen
  at high zoom would wrap and draw a line across the display. Route segments and the return tether are now clipped
  (Liang-Barsky) before they are narrowed. Covered by the host test.
* Tile loading is shared: skips the header, byte-swaps for `LV_COLOR_16_SWAP`, expands to BGRA for 32-bit.
  A truncated tile shows flat colour in its tail instead of garbage.
* Platform layer (`map_platform.*`): logging, allocation with PSRAM-or-heap fallback, pluggable file access.
* Default loader stack 6 KB (was 4 KB) since it also formats log lines; legacy grid buffers no longer insist on
  `MALLOC_CAP_DMA`.

## Layout

```
src/            the library (MapTiles.h is the umbrella header)
examples/       TileGrid, MapViewFollow, Navigation
scripts/        PNG -> .bin tile converter (unchanged from upstream)
extras/host_test/   PC-side test harness, run with ./run.sh
```

Route files (`route.bin`, magic `MRT1`) are described byte-for-byte at the top of `src/map_route.h`. Upstream
names its own exporter tooling as the producer; `extras/host_test/gen_data.py` has a minimal writer if you need
one for testing.
