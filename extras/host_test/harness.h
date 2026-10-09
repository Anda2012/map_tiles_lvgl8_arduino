/* Shared scaffolding for the rotation tests: 480x480 RGB565 framebuffer behind a
 * 48-line partial draw buffer (10 draw chunks per frame, like a small MCU). */
#pragma once
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "lvgl.h"
#include "MapTiles.h"

#define W 480
#define H 480
static uint16_t fb[W * H];
static lv_color_t hbuf[W * 48];
static lv_disp_draw_buf_t hdraw;
static lv_disp_drv_t hdisp;
static lv_indev_drv_t hindev;
static lv_point_t ptr = {0, 0};
static lv_indev_state_t ptr_state = LV_INDEV_STATE_RELEASED;

static void h_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c)
{
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++, c++) {
#if LV_COLOR_DEPTH == 16
  #if LV_COLOR_16_SWAP
            fb[y * W + x] = (uint16_t)((c->full >> 8) | (c->full << 8));
  #else
            fb[y * W + x] = c->full;
  #endif
#else
            uint32_t v = c->full; uint8_t r=(v>>16)&255,g=(v>>8)&255,b=v&255;
            fb[y * W + x] = ((r>>3)<<11)|((g>>2)<<5)|(b>>3);
#endif
        }
    lv_disp_flush_ready(d);
}
static void h_ptr(lv_indev_drv_t *d, lv_indev_data_t *data) { (void)d; data->point = ptr; data->state = ptr_state; }

static lv_obj_t *harness_init(void)
{
    lv_init();
    lv_disp_draw_buf_init(&hdraw, hbuf, NULL, W * 48);
    lv_disp_drv_init(&hdisp);
    hdisp.hor_res = W; hdisp.ver_res = H; hdisp.flush_cb = h_flush; hdisp.draw_buf = &hdraw;
    lv_disp_drv_register(&hdisp);
    lv_indev_drv_init(&hindev); hindev.type = LV_INDEV_TYPE_POINTER; hindev.read_cb = h_ptr;
    lv_indev_drv_register(&hindev);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}
static void spin(int ms) { for (int i = 0; i < ms / 10; i++) { lv_tick_inc(10); lv_timer_handler(); usleep(10000); } }
static void dump(const char *name)
{
    char p[160]; snprintf(p, sizeof p, "out/%s.ppm", name);
    FILE *f = fopen(p, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { uint16_t v = fb[i]; uint8_t rgb[3] = { ((v>>11)&31)<<3, ((v>>5)&63)<<2, (v&31)<<3 }; fwrite(rgb, 1, 3, f); }
    fclose(f);
}
static bool near_color(uint16_t px, uint32_t hex)
{
    int r = ((px >> 11) & 31) << 3, g = ((px >> 5) & 63) << 2, b = (px & 31) << 3;
    int R = (hex >> 16) & 255, G = (hex >> 8) & 255, B = hex & 255;
    return abs(r - R) < 12 && abs(g - G) < 8 && abs(b - B) < 12;
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); } else { printf("  ok:   "); } printf(__VA_ARGS__); printf("\n"); } while (0)
