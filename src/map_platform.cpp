#include "map_platform.h"

#include <stdarg.h>

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#if defined(__has_include)
#if __has_include(<dirent.h>)
#include <dirent.h>
#define MAP_HAVE_DIRENT 1
#endif
#endif

/* ------------------------------------------------------------------- logging */

static map_tiles_log_fn_t s_log_fn = NULL;

extern "C" void map_tiles_set_log_handler(map_tiles_log_fn_t fn)
{
    s_log_fn = fn;
}

extern "C" void map_tiles_log(int level, const char *tag, const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (s_log_fn) {
        s_log_fn(level, tag, msg);
        return;
    }

    static const char letters[] = { '?', 'E', 'W', 'I', 'D' };
    char c = (level >= 1 && level <= 4) ? letters[level] : '?';
#if defined(ARDUINO)
    Serial.print('[');
    Serial.print(c);
    Serial.print("][map_tiles/");
    Serial.print(tag);
    Serial.print("] ");
    Serial.println(msg);
#else
    fprintf(stderr, "[%c][map_tiles/%s] %s\n", c, tag, msg);
#endif
}

/* ---------------------------------------------------------------- file access */

static void *stdio_open(const char *path, void *)               { return fopen(path, "rb"); }
static size_t stdio_read(void *f, void *b, size_t n, void *)    { return fread(b, 1, n, (FILE *)f); }
static bool stdio_seek(void *f, uint32_t pos, void *)           { return fseek((FILE *)f, (long)pos, SEEK_SET) == 0; }
static void stdio_close(void *f, void *)                        { fclose((FILE *)f); }

static int stdio_list(const char *path, map_tiles_name_cb_t cb, void *ctx, void *)
{
#if defined(MAP_HAVE_DIRENT)
    DIR *dir = opendir(path);
    if (!dir) return -1;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) cb(e->d_name, ctx);
    closedir(dir);
    return 0;
#else
    (void)path; (void)cb; (void)ctx;
    return -1;
#endif
}

static const map_tiles_fs_t k_stdio_fs = {
    stdio_open, stdio_read, stdio_seek, stdio_close, stdio_list, NULL
};

static map_tiles_fs_t s_fs = k_stdio_fs;

extern "C" void map_tiles_set_fs(const map_tiles_fs_t *fs)
{
    if (fs && fs->open && fs->read && fs->seek && fs->close) s_fs = *fs;
    else                                                       s_fs = k_stdio_fs;
}

extern "C" void *map_fs_open(const char *path)                    { return s_fs.open(path, s_fs.user); }
extern "C" size_t map_fs_read(void *f, void *b, size_t n)         { return s_fs.read(f, b, n, s_fs.user); }
extern "C" bool map_fs_seek(void *f, uint32_t pos)                { return s_fs.seek(f, pos, s_fs.user); }
extern "C" void map_fs_close(void *f)                             { s_fs.close(f, s_fs.user); }

extern "C" int map_fs_list_dir(const char *path, map_tiles_name_cb_t cb, void *ctx)
{
    if (!s_fs.list_dir) return -1;
    return s_fs.list_dir(path, cb, ctx, s_fs.user);
}

/* ---------------------------------------------------------------- tile loader */

extern "C" map_tile_status_t map_tile_load_file(const char *path, uint8_t *dst)
{
    void *f = map_fs_open(path);
    if (!f) return MAP_TILE_MISSING;

    if (!map_fs_seek(f, MAP_TILE_FILE_HEADER)) {
        map_fs_close(f);
        return MAP_TILE_MISSING;
    }

#if LV_COLOR_DEPTH == 16
    size_t got = map_fs_read(f, dst, MAP_TILE_FILE_BYTES);
    map_fs_close(f);

    size_t valid = (got / 2) * 2;
#if LV_COLOR_16_SWAP
    uint16_t *px = (uint16_t *)dst;
    for (size_t i = 0; i < valid / 2; i++) {
        px[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));
    }
#endif
    if (valid < MAP_TILE_FILE_BYTES) memset(dst + valid, 0, MAP_TILE_FILE_BYTES - valid);
    return (got == MAP_TILE_FILE_BYTES) ? MAP_TILE_OK : MAP_TILE_SHORT;

#else /* LV_COLOR_DEPTH == 32: expand RGB565 -> BGRA in small chunks */
    uint8_t chunk[512];
    uint8_t *out = dst;
    size_t done = 0;
    bool complete = true;

    while (done < MAP_TILE_PIXELS) {
        size_t want = (MAP_TILE_PIXELS - done) * 2;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        size_t n = map_fs_read(f, chunk, want);
        size_t npx = n / 2;

        for (size_t i = 0; i < npx; i++) {
            uint16_t v = (uint16_t)(chunk[2 * i] | (chunk[2 * i + 1] << 8));
            uint8_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            out[0] = (uint8_t)((b << 3) | (b >> 2));
            out[1] = (uint8_t)((g << 2) | (g >> 4));
            out[2] = (uint8_t)((r << 3) | (r >> 2));
            out[3] = 0xFF;
            out += 4;
        }
        done += npx;
        if (n < want) { complete = false; break; }
    }
    map_fs_close(f);

    if (done < MAP_TILE_PIXELS) memset(out, 0, (MAP_TILE_PIXELS - done) * 4);
    return complete ? MAP_TILE_OK : MAP_TILE_SHORT;
#endif
}
