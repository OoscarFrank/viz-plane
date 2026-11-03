/* Simulator: 800x480 SDL window, the mouse acts as a finger.
 * Options: --wifi                start with no saved network (Wi-Fi screen)
 *          --shot F --after MS   save a BMP screenshot, then quit
 *          --tap X,Y@MS          simulate a tap (repeatable)
 *          --demo [file]         frozen traffic (default: demo_aircraft.json) */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "plat.h"
#include "radar_app.h"
#include "sim_tick.h"

void sim_plat_init(bool wifi_setup, const char *demo_file);

#define W 800
#define H 480
static uint16_t fb[W * H];
static bool dirty = true;
static int mx, my;
static bool mdown;

typedef struct { int x, y; uint32_t at; } tap_t;
static tap_t taps[16];
static int ntaps;

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px)
{
    for (int y = a->y1; y <= a->y2; y++)
        memcpy(&fb[y * W + a->x1], &px[(y - a->y1) * (a->x2 - a->x1 + 1)], (a->x2 - a->x1 + 1) * 2);
    dirty = true;
    lv_disp_flush_ready(d);
}

static void read_mouse(lv_indev_drv_t *d, lv_indev_data_t *data)
{
    uint32_t now = sim_tick_ms();
    for (int i = 0; i < ntaps; i++)
        if (now >= taps[i].at && now < taps[i].at + 120) { data->point.x = taps[i].x; data->point.y = taps[i].y; data->state = LV_INDEV_STATE_PR; return; }
    data->point.x = mx; data->point.y = my;
    data->state = mdown ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

int main(int argc, char **argv)
{
    bool wifi = false;
    const char *demo = NULL;
    const char *shot = NULL;
    uint32_t after = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wifi")) wifi = true;
        else if (!strcmp(argv[i], "--demo")) demo = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "demo_aircraft.json";
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--after") && i + 1 < argc) after = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tap") && i + 1 < argc && ntaps < 16) {
            tap_t *t = &taps[ntaps++];
            sscanf(argv[++i], "%d,%d@%u", &t->x, &t->y, &t->at);
        }
    }
    sim_plat_init(wifi, demo);
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *win = SDL_CreateWindow("Mulhouse-Basel radar (simulator)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                                       shot ? SDL_WINDOW_HIDDEN : 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, W, H);

    lv_init();
    static lv_color_t buf[W * 60];
    static lv_disp_draw_buf_t db;
    lv_disp_draw_buf_init(&db, buf, NULL, W * 60);
    static lv_disp_drv_t dd;
    lv_disp_drv_init(&dd);
    dd.hor_res = W; dd.ver_res = H; dd.flush_cb = flush; dd.draw_buf = &db;
    lv_disp_drv_register(&dd);
    static lv_indev_drv_t id;
    lv_indev_drv_init(&id);
    id.type = LV_INDEV_TYPE_POINTER; id.read_cb = read_mouse;
    lv_indev_drv_register(&id);

    plat_lock(-1);
    radar_app_start();
    plat_unlock();

    for (bool run = true; run;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) run = false;
            if (e.type == SDL_MOUSEMOTION) { mx = e.motion.x; my = e.motion.y; }
            if (e.type == SDL_MOUSEBUTTONDOWN) { mdown = true; mx = e.button.x; my = e.button.y; }
            if (e.type == SDL_MOUSEBUTTONUP) mdown = false;
        }
        plat_lock(-1);
        lv_timer_handler();
        plat_unlock();
        if (dirty) {
            SDL_UpdateTexture(tex, NULL, fb, W * 2);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
            dirty = false;
        }
        if (shot && sim_tick_ms() > after) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(fb, W, H, 16, W * 2, SDL_PIXELFORMAT_RGB565);
            SDL_SaveBMP(s, shot);
            SDL_FreeSurface(s);
            run = false;
        }
        SDL_Delay(5);
    }
    SDL_Quit();
    return 0;
}
