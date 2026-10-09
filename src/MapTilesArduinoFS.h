/**
 * @file MapTilesArduinoFS.h
 * @brief Optional: read tiles and routes through an Arduino `fs::FS` (SD, SD_MMC, LittleFS...).
 *
 * You normally do NOT need this on ESP32: the default stdio access already reaches
 * those file systems through their VFS mount points ("/sd", "/sdcard", "/littlefs").
 * Use it when you would rather pass the FS object itself, or on a core that has an
 * `FS.h` but no POSIX file layer.
 *
 * @code
 * #include <SD.h>
 * #include <MapTiles.h>
 * #include <MapTilesArduinoFS.h>
 *
 * SD.begin(CS_PIN);
 * map_tiles_use_arduino_fs(SD);                       // paths are now relative to the FS root
 * map_view_set_tile_source(map, "", "tiles1", 32, true);   // -> "/tiles1/16/x/y.bin"
 * @endcode
 *
 * Paths handed to the library are "<base_path>/<folder>/...". With an FS the base
 * path may be "" (root) or e.g. "/maps". The FS object must outlive every view.
 * Include this header from one .cpp / .ino only.
 */
#pragma once

#include "MapTiles.h"

#if defined(__has_include)
#if __has_include(<FS.h>)
#include <FS.h>
#define MAP_TILES_HAVE_ARDUINO_FS 1
#endif
#endif

#if defined(MAP_TILES_HAVE_ARDUINO_FS)

namespace map_tiles_detail {

static inline const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static inline void *fs_open(const char *path, void *user)
{
    fs::FS *fs = (fs::FS *)user;
    File f = fs->open(path, "r");
    if (!f || f.isDirectory()) return NULL;
    return new File(f);
}

static inline size_t fs_read(void *file, void *buf, size_t len, void *)
{
    return ((File *)file)->read((uint8_t *)buf, len);
}

static inline bool fs_seek(void *file, uint32_t pos, void *)
{
    return ((File *)file)->seek(pos);
}

static inline void fs_close(void *file, void *)
{
    File *f = (File *)file;
    f->close();
    delete f;
}

static inline int fs_list(const char *path, map_tiles_name_cb_t cb, void *ctx, void *user)
{
    fs::FS *fs = (fs::FS *)user;
    File dir = fs->open(path);
    if (!dir || !dir.isDirectory()) return -1;

    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        /* name() is the bare name on recent cores and the full path on old ones. */
        cb(basename_of(e.name()), ctx);
        e.close();
    }
    dir.close();
    return 0;
}

} // namespace map_tiles_detail

/** @brief Route all tile / route file access through @p fs. Call before creating a view. */
static inline void map_tiles_use_arduino_fs(fs::FS &fs)
{
    map_tiles_fs_t hooks;
    hooks.open     = map_tiles_detail::fs_open;
    hooks.read     = map_tiles_detail::fs_read;
    hooks.seek     = map_tiles_detail::fs_seek;
    hooks.close    = map_tiles_detail::fs_close;
    hooks.list_dir = map_tiles_detail::fs_list;
    hooks.user     = &fs;
    map_tiles_set_fs(&hooks);
}

#else
#error "MapTilesArduinoFS.h needs an Arduino core that provides <FS.h>"
#endif
