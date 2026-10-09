#!/bin/bash
# Host-side verification: builds real LVGL 8.3 + this library on a PC, renders the map
# view into a RAM framebuffer (with a small partial draw buffer, like an MCU) and checks
# the output. No hardware, no Arduino core needed. Needs: gcc, g++, python3 + Pillow, git.
#
#   ./run.sh            all three colour setups (16 bit, 16 bit + LV_COLOR_16_SWAP, 32 bit)
#   ./run.sh d16        just one of: d16 | swap | d32
#   LVGL_DIR=/path/to/lvgl-8.3 ./run.sh   reuse a checkout instead of cloning
set -e
cd "$(dirname "$0")"
HERE=$PWD; SRC=$HERE/../../src; B=$HERE/build
LVGL=${LVGL_DIR:-$B/lvgl8}
mkdir -p "$B/out"

[ -d "$LVGL/src" ] || git clone --depth 1 -b release/v8.3 https://github.com/lvgl/lvgl.git "$LVGL"

# lv_conf.h from LVGL's template; colour depth / swap selectable from the command line
sed -e 's/#if 0 /#if 1 /' \
    -e 's/#define LV_COLOR_DEPTH 16/#ifndef LV_COLOR_DEPTH\n#define LV_COLOR_DEPTH 16\n#endif/' \
    -e 's/#define LV_COLOR_16_SWAP 0/#ifndef LV_COLOR_16_SWAP\n#define LV_COLOR_16_SWAP 0\n#endif/' \
    -e 's/#define LV_MEM_SIZE (48U \* 1024U)/#define LV_MEM_SIZE (2048U * 1024U)/' \
    "$LVGL/lv_conf_template.h" > "$B/lv_conf.h"

python3 gen_data.py            # tiles + route.bin -> /tmp/maptest (through the real converter script)

run_one() {   # tag, flags...
  tag=$1; shift
  if [ ! -f "$B/liblvgl_$tag.a" ]; then
    mkdir -p "$B/obj_$tag"
    find "$LVGL/src" -name '*.c' | xargs -P "$(nproc)" -I{} bash -c \
      'gcc -c -O1 -w -I"'$B'" -I"'$LVGL'" -DLV_CONF_INCLUDE_SIMPLE '"$*"' {} -o "'$B'/obj_'$tag'/$(echo {} | md5sum | cut -c1-12).o"'
    ar rcs "$B/liblvgl_$tag.a" "$B"/obj_$tag/*.o
  fi
  gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
      -I"$B" -I"$HERE/shim" -I"$LVGL" -I"$SRC" -DLV_CONF_INCLUDE_SIMPLE "$@" \
      test.c "$SRC/map_geo.c" "$SRC/map_route.c" "$SRC/map_tile_cache.c" "$SRC/map_view.c" \
      -x c++ "$SRC/map_tiles.cpp" "$SRC/map_platform.cpp" -x none \
      "$B/liblvgl_$tag.a" -lm -lpthread -o "$B/test_$tag"
  gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
      -I"$B" -I"$HERE" -I"$HERE/shim" -I"$LVGL" -I"$SRC" -DLV_CONF_INCLUDE_SIMPLE "$@" \
      test_rot.c "$SRC/map_geo.c" "$SRC/map_route.c" "$SRC/map_tile_cache.c" "$SRC/map_view.c" \
      -x c++ "$SRC/map_tiles.cpp" "$SRC/map_platform.cpp" -x none \
      "$B/liblvgl_$tag.a" -lm -lpthread -o "$B/test_rot_$tag"
  echo "=============== $tag ($*)  [functional]"
  (cd "$B" && ./test_$tag)
  echo "=============== $tag ($*)  [rotation]"
  (cd "$B" && ./test_rot_$tag)
}

want=${1:-all}
[ "$want" = all ] || [ "$want" = d16 ]  && run_one d16
[ "$want" = all ] || [ "$want" = swap ] && run_one swap -DLV_COLOR_16_SWAP=1
[ "$want" = all ] || [ "$want" = d32 ]  && run_one d32  -DLV_COLOR_DEPTH=32
echo; echo "Screenshots (PPM) are in $B/out"
