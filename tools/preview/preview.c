// Renders main/ui.c through real LVGL and saves RGB565 frames, so the screen can
// be checked without the board. tools/preview/run.sh turns them into docs/*.png.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "ui.h"

#define W 466
#define H 466
static uint16_t s_fb[W * H];
static long s_px_sum, s_frames_counted;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)px;
    s_px_sum += lv_area_get_size(a);
    lv_display_flush_ready(d);
}

static uint32_t s_ms;
static uint32_t tick_cb(void)
{
    return s_ms;
}

static void step(const ui_state_t *s)
{
    ui_update(s, 0.033f);
    s_ms += 33;
    lv_timer_handler();
    s_frames_counted++;
}

static void save(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "build_host/frames/%s.rgb565", name);
    FILE *f = fopen(path, "wb");
    fwrite(s_fb, 2, W * H, f);
    fclose(f);
    printf("%s\n", name);
}

static float wobble(int i, float amount)
{
    return amount * (float)(((i * 7919) % 200) - 100) / 100.0f;
}

// Hear a sound from one direction for a while.
static void hear(ui_state_t *s, float angle, float conf, float seconds)
{
    int n = (int)(seconds / 0.033f);
    for (int i = 0; i < n; i++) {
        s->tracking = true;
        s->live = (i % 3) != 2;
        s->angle_deg = angle + wobble(i, 2.0f);
        s->confidence = conf;
        s->age_s = 0;
        s->tau_us = 66.2f * sinf(angle * 3.14159f / 180.0f);
        s->level_db[0] = -34.0f + wobble(i, 3.0f) - angle * 0.03f;
        s->level_db[1] = -33.0f + wobble(i + 3, 3.0f) + angle * 0.03f;
        step(s);
    }
}

static void quiet(ui_state_t *s, float seconds, bool still_tracking)
{
    int n = (int)(seconds / 0.033f);
    for (int i = 0; i < n; i++) {
        s->live = false;
        s->tracking = still_tracking;
        s->age_s += 0.033f;
        s->level_db[0] = -58.0f + wobble(i, 1.5f);
        s->level_db[1] = -57.0f + wobble(i + 5, 1.5f);
        step(s);
    }
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_fb, NULL, sizeof(s_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);

    ui_init(lv_screen_active(), NULL);
    ui_state_t s;
    memset(&s, 0, sizeof(s));

    quiet(&s, 1.2f, false);
    save("1_listening");

    hear(&s, -52, 0.6f, 1.2f);
    hear(&s, 34, 0.93f, 1.6f);
    save("2_right");

    long px0 = s_px_sum, fr0 = s_frames_counted;
    hear(&s, 34, 0.93f, 3.0f);
    printf("steady tracking redraws %.1f%% of the screen per frame\n",
           100.0 * (double)(s_px_sum - px0) / (double)(s_frames_counted - fr0) / (W * H));

    hear(&s, -61, 0.46f, 1.4f);
    save("3_left_unsure");

    hear(&s, 3, 0.9f, 1.4f);
    save("4_ahead");

    s.age_s = 0;
    quiet(&s, 1.4f, true);
    save("5_last_heard");

    quiet(&s, 3.0f, false);
    ui_set_notice("ZEROING", "clap straight ahead");
    quiet(&s, 0.5f, false);
    save("6_zeroing");
    return 0;
}
