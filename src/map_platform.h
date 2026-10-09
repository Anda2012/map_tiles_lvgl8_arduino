/**
 * @file map_platform.h
 * @brief Platform glue for map_tiles_lvgl8: logging, memory, file access and
 *        the tile pixel loader.
 *
 * This is what replaces the ESP-IDF specific parts of the upstream component
 * (esp_log, heap_caps, the VFS-only file access). Everything here works on
 * Arduino-ESP32 and PlatformIO out of the box; other FreeRTOS targets can plug
 * in their own file access with ::map_tiles_set_fs.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "lvgl.h"

#if !defined(LVGL_VERSION_MAJOR) || (LVGL_VERSION_MAJOR != 8) || (LVGL_VERSION_MINOR < 2)
#error "map_tiles_lvgl8 needs LVGL 8.2 or newer in the 8.x series (8.3.x recommended). For LVGL 9 use the upstream 0015/map_tiles component."
#endif

#if (LV_COLOR_DEPTH != 16) && (LV_COLOR_DEPTH != 32)
#error "map_tiles_lvgl8 supports LV_COLOR_DEPTH 16 and 32 only (set it in lv_conf.h)."
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#define MAP_TILES_PLATFORM_ESP32 1
#include "esp_heap_caps.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- tile format */

#define MAP_TILE_PX             256
#define MAP_TILE_PIXELS         (MAP_TILE_PX * MAP_TILE_PX)
#define MAP_TILE_FILE_HEADER    12                              /**< LVGL 9 .bin header, skipped */
#define MAP_TILE_FILE_BYTES     (MAP_TILE_PIXELS * 2)           /**< RGB565 payload on the card */
#define MAP_TILE_BYTES_PER_PX   (LV_COLOR_DEPTH / 8)            /**< In RAM: 2 (16 bit) or 4 (32 bit) */
#define MAP_TILE_RAM_BYTES      (MAP_TILE_PIXELS * MAP_TILE_BYTES_PER_PX)

typedef enum {
    MAP_TILE_MISSING = 0,   /**< Could not be opened */
    MAP_TILE_OK,            /**< Fully loaded */
    MAP_TILE_SHORT,         /**< Truncated file; the tail was blanked */
} map_tile_status_t;

/**
 * @brief Read one tile file into @p dst (MAP_TILE_RAM_BYTES bytes).
 *
 * Skips the 12 byte header and converts the RGB565 little-endian payload to
 * whatever LVGL is built for: copied as is for LV_COLOR_DEPTH 16, byte swapped
 * when LV_COLOR_16_SWAP is set, expanded to BGRA for LV_COLOR_DEPTH 32.
 */
map_tile_status_t map_tile_load_file(const char *path, uint8_t *dst);

/* ------------------------------------------------------------------- logging */

/** 0 off, 1 errors, 2 warnings (default), 3 info, 4 debug. Override with -D. */
#ifndef MAP_TILES_LOG_LEVEL
#define MAP_TILES_LOG_LEVEL 2
#endif

typedef void (*map_tiles_log_fn_t)(int level, const char *tag, const char *msg);

/** @brief Route library messages somewhere else than Serial. NULL restores the default. */
void map_tiles_set_log_handler(map_tiles_log_fn_t fn);

void map_tiles_log(int level, const char *tag, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define MAP_LOGE(tag, ...) do { if (MAP_TILES_LOG_LEVEL >= 1) map_tiles_log(1, tag, __VA_ARGS__); } while (0)
#define MAP_LOGW(tag, ...) do { if (MAP_TILES_LOG_LEVEL >= 2) map_tiles_log(2, tag, __VA_ARGS__); } while (0)
#define MAP_LOGI(tag, ...) do { if (MAP_TILES_LOG_LEVEL >= 3) map_tiles_log(3, tag, __VA_ARGS__); } while (0)
#define MAP_LOGD(tag, ...) do { if (MAP_TILES_LOG_LEVEL >= 4) map_tiles_log(4, tag, __VA_ARGS__); } while (0)

/* -------------------------------------------------------------------- memory */

/**
 * @brief Allocate a big buffer, preferring PSRAM when asked.
 *
 * Falls back to the normal heap when there is no PSRAM, so use_spiram = true is
 * harmless on a board without it (it will simply run out of RAM sooner).
 */
static inline void *map_alloc(size_t size, bool prefer_spiram)
{
    if (size == 0) return NULL;
#if defined(MAP_TILES_PLATFORM_ESP32)
    void *p = NULL;
    if (prefer_spiram) p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return p;
#else
    (void)prefer_spiram;
    return malloc(size);
#endif
}

static inline void map_free(void *p)
{
#if defined(MAP_TILES_PLATFORM_ESP32)
    heap_caps_free(p);
#else
    free(p);
#endif
}

/* ---------------------------------------------------------------- file access */

typedef void (*map_tiles_name_cb_t)(const char *name, void *ctx);

/**
 * @brief File access hooks.
 *
 * The default uses stdio, which on Arduino-ESP32 reaches SD ("/sd"), SD_MMC
 * ("/sdcard"), LittleFS ("/littlefs") and SPIFFS ("/spiffs") through the VFS.
 * Provide your own on cores without that. Every hook is called from the tile
 * loader task as well as the LVGL thread, so it must be thread safe. LVGL's own
 * lv_fs is deliberately not used: its drivers allocate from LVGL's heap, which
 * is not safe to touch from a second task.
 */
typedef struct {
    void  *(*open)(const char *path, void *user);                      /**< NULL if missing */
    size_t (*read)(void *file, void *buf, size_t len, void *user);
    bool   (*seek)(void *file, uint32_t pos, void *user);              /**< From the start */
    void   (*close)(void *file, void *user);
    /** Call @p cb for every entry of a directory; return <0 if it cannot be opened. May be NULL. */
    int    (*list_dir)(const char *path, map_tiles_name_cb_t cb, void *ctx, void *user);
    void   *user;
} map_tiles_fs_t;

/** @brief Install file hooks (copied). Call before creating any view. NULL restores stdio. */
void map_tiles_set_fs(const map_tiles_fs_t *fs);

void  *map_fs_open(const char *path);
size_t map_fs_read(void *file, void *buf, size_t len);
bool   map_fs_seek(void *file, uint32_t pos);
void   map_fs_close(void *file);
int    map_fs_list_dir(const char *path, map_tiles_name_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
