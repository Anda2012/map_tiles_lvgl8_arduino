# Host test

`./run.sh` compiles real LVGL 8.3 plus this library on a PC, with a pthread-based FreeRTOS shim
(`shim/`) standing in for the RTOS, and drives the library through:

* route peek/load, map matching (on route, off route with frozen progress, arrival, next maneuver)
* the `map_view` widget rendering into a framebuffer through a **partial** draw buffer (like an MCU),
  incl. tile placement vs. route alignment, route casing, off-route tether, drag takes over from follow
* a 15 km segment at z19, which would wrap `int16` coordinates without the clipping in `map_view.c`
* the legacy `map_tiles_*` grid API
* truncated / missing tiles and custom file hooks
* rotation (`test_rot.c`): rotated tiles vs. the vector layer (the road in the tile must sit where the
  library's own maths says), seams between rotated tiles on flat-colour tiles, screen<->lat/lon round trips,
  dragging at any angle, heading-up easing, and a frame-cost comparison
* all of the above with `LV_COLOR_DEPTH 16`, `16` + `LV_COLOR_16_SWAP`, and `32`

Tiles are produced from PNGs by `scripts/lvgl_map_tile_converter.py`, so the real tile format is exercised.
`gen_data.py` also contains a minimal writer for the `route.bin` (MRT1) format.

This verifies the logic and the LVGL 8 draw calls. It is **not** a substitute for running on a board:
PSRAM, SD/SPI timing and your display driver are out of its reach.
