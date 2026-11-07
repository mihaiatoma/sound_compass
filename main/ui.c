// Sound compass display, 466 x 466 round AMOLED.
//
// The top half is the listening field: 0 degrees straight up, 90 at each side.
// The bottom half is the mirror of the top, because two mics cannot tell front
// from back, and it carries the mic levels. The two rings near the lower edge sit
// over the real microphones, drawn to scale from the board's 3D model.
//
// Colour means side: warm is left, cool is right, white is straight ahead.
// Beam width means certainty: narrow is sure, wide is unsure.
//
// Everything moving is drawn in one layer that redraws only the rectangles that
// changed, because a full-screen redraw over QSPI would cost far more time.
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(font_digits_124);
LV_FONT_DECLARE(font_word_28);
LV_FONT_DECLARE(font_small_21);

#define SIZE 466
#define CX   233
#define CY   233

#define COL_LEFT     0xFF8A4C
#define COL_RIGHT    0x4CC3FF
#define COL_CENTRE   0xF4F6F7
#define COL_TEXT_DIM 0x828E97
#define COL_TICK     0x4A545C
#define COL_TICK_LOW 0x262D33

#define TICK_OUTER   230
#define TICK_MINOR   9
#define TICK_MAJOR   18
#define BEAM_OUTER   208
#define BEAM_WIDTH   8
#define NEEDLE_FROM  128
#define NEEDLE_TO    190
#define HEAT_BASE    184
#define HEAT_LENGTH  38
#define HEAT_BINS    37            // one bar every 5 degrees
#define LEVEL_RADIUS 188
#define LEVEL_WIDTH  5
#define LEVEL_SPAN   66.0f
#define RING_MIN     118.0f
#define RING_MAX     150.0f

// Mic sound ports projected on the panel: 10.65 px/mm from the 43.76 mm active
// area, using the MIC1 and MIC2 positions in Waveshare's STEP model.
#define MIC_L_X 116
#define MIC_R_X 358
#define MIC_Y   419

#define DEG(x) ((float)(x) * 3.14159265f / 180.0f)
#define PI2 6.2831853f

static struct {
    lv_obj_t *field;
    lv_obj_t *digits, *degree, *word, *detail, *status;
    lv_obj_t *title, *subtitle;
    lv_obj_t *db[2];
    ui_long_press_cb_t on_long_press;

    bool  has_beam;
    bool  notice;
    float angle;
    float spread;
    float fade;
    lv_area_t beam_area, ghost_area, ring_area;
    float heat[HEAT_BINS];
    float level[2];
    float ring_radius;
    float breath;

    int   shown_degrees;
    float text_timer;
    float db_timer;
    float decay_timer;
    bool  idle_shown;
} ui;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Bearing to screen position: 0 is up, positive is clockwise.
static void polar(float bearing, float r, int32_t *x, int32_t *y)
{
    *x = (int32_t)lroundf((float)CX + r * sinf(DEG(bearing)));
    *y = (int32_t)lroundf((float)CY - r * cosf(DEG(bearing)));
}

static lv_color_t side_color(float angle)
{
    float t = clampf(fabsf(angle) / 34.0f, 0.0f, 1.0f);
    lv_color_t side = lv_color_hex(angle < 0 ? COL_LEFT : COL_RIGHT);
    return lv_color_mix(side, lv_color_hex(COL_CENTRE), (uint8_t)(t * 255.0f));
}

static void area_grow(lv_area_t *a, int32_t x, int32_t y)
{
    if (x < a->x1) a->x1 = x;
    if (x > a->x2) a->x2 = x;
    if (y < a->y1) a->y1 = y;
    if (y > a->y2) a->y2 = y;
}

static void sector_area(float from, float to, float r_in, float r_out, lv_area_t *a)
{
    int32_t x, y;
    polar(from, r_in, &x, &y);
    a->x1 = a->x2 = x;
    a->y1 = a->y2 = y;
    for (int i = 0; i <= 8; i++) {
        float b = from + (to - from) * (float)i / 8.0f;
        polar(b, r_in, &x, &y);
        area_grow(a, x, y);
        polar(b, r_out, &x, &y);
        area_grow(a, x, y);
    }
    a->x1 -= 5; a->y1 -= 5;
    a->x2 += 5; a->y2 += 5;
}

static void draw_radial(lv_layer_t *layer, lv_draw_line_dsc_t *d, float bearing, float r_in, float r_out)
{
    int32_t x, y;
    polar(bearing, r_in, &x, &y);
    d->p1.x = x; d->p1.y = y;
    polar(bearing, r_out, &x, &y);
    d->p2.x = x; d->p2.y = y;
    lv_draw_line(layer, d);
}

static void draw_arc(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t radius, int32_t width,
                     float start, float end, lv_color_t color, lv_opa_t opa, bool rounded)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx;
    d.center.y = cy;
    d.radius = (uint16_t)radius;
    d.width = width;
    d.start_angle = (lv_value_precise_t)fmodf(start + 720.0f, 360.0f);
    d.end_angle = (lv_value_precise_t)fmodf(end + 720.0f, 360.0f);
    d.color = color;
    d.opa = opa;
    d.rounded = rounded;
    lv_draw_arc(layer, &d);
}

static void draw_scale(lv_layer_t *layer)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.round_start = 1;
    d.round_end = 1;

    for (int a = -90; a <= 90; a += 5) {
        bool major = (a % 30) == 0;
        d.width = major ? 3 : 2;
        d.color = lv_color_hex(major ? 0x8A96A0 : COL_TICK);
        draw_radial(layer, &d, (float)a, (float)(TICK_OUTER - (major ? TICK_MAJOR : TICK_MINOR)), (float)TICK_OUTER);
    }
    d.width = 2;
    d.color = lv_color_hex(COL_TICK_LOW);
    for (int a = 105; a <= 255; a += 15) {
        draw_radial(layer, &d, (float)a, (float)(TICK_OUTER - TICK_MINOR), (float)TICK_OUTER);
    }
}

static void draw_heat(lv_layer_t *layer)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.round_start = 1;
    d.round_end = 1;

    for (int i = 0; i < HEAT_BINS; i++) {
        float bearing = -90.0f + 5.0f * (float)i;
        float h = ui.heat[i];
        if (h < 0.03f) {
            d.width = 3;
            d.opa = LV_OPA_COVER;
            d.color = lv_color_hex(COL_TICK_LOW);
            draw_radial(layer, &d, bearing, (float)HEAT_BASE - 1.0f, (float)HEAT_BASE);
        } else {
            d.width = 6;
            d.opa = (lv_opa_t)(70.0f + 185.0f * h);
            d.color = side_color(bearing);
            draw_radial(layer, &d, bearing, (float)HEAT_BASE - h * (float)HEAT_LENGTH, (float)HEAT_BASE);
        }
    }
}

static void draw_levels(lv_layer_t *layer)
{
    static const float start[2] = {102.0f, 78.0f};
    static const int32_t mic_x[2] = {MIC_L_X, MIC_R_X};
    for (int m = 0; m < 2; m++) {
        lv_color_t c = lv_color_hex(m == 0 ? COL_LEFT : COL_RIGHT);
        float dir = m == 0 ? 1.0f : -1.0f;
        float a0 = start[m], a1 = a0 + dir * LEVEL_SPAN;
        float fill = a0 + dir * LEVEL_SPAN * ui.level[m];
        float lo = dir > 0 ? a0 : a1, hi = dir > 0 ? a1 : a0;
        draw_arc(layer, CX, CY, LEVEL_RADIUS, LEVEL_WIDTH, lo, hi, lv_color_hex(COL_TICK_LOW), LV_OPA_COVER, true);
        if (ui.level[m] > 0.01f) {
            float flo = dir > 0 ? a0 : fill, fhi = dir > 0 ? fill : a0;
            draw_arc(layer, CX, CY, LEVEL_RADIUS, LEVEL_WIDTH, flo, fhi, c, LV_OPA_COVER, true);
        }
        draw_arc(layer, mic_x[m], MIC_Y, 8, 2, 0.0f, 359.9f, c, (lv_opa_t)(90.0f + 165.0f * ui.level[m]), false);
    }
}

static void draw_beam(lv_layer_t *layer)
{
    if (!ui.has_beam) {
        return;
    }
    float a = 270.0f + ui.angle, s = ui.spread;
    float ghost = 90.0f - ui.angle;
    lv_color_t c = side_color(ui.angle);
    float f = ui.fade;

    draw_arc(layer, CX, CY, BEAM_OUTER - 2, 3, ghost - s, ghost + s, c, (lv_opa_t)(70.0f * f), true);

    draw_arc(layer, CX, CY, BEAM_OUTER + 9, BEAM_WIDTH + 18, a - s - 2, a + s + 2, c, (lv_opa_t)(22.0f * f), true);
    draw_arc(layer, CX, CY, BEAM_OUTER + 4, BEAM_WIDTH + 8, a - s - 1, a + s + 1, c, (lv_opa_t)(46.0f * f), true);
    draw_arc(layer, CX, CY, BEAM_OUTER, BEAM_WIDTH, a - s, a + s, c, (lv_opa_t)(255.0f * f), true);

    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.width = 2;
    d.round_start = 1;
    d.round_end = 1;
    d.color = c;
    d.opa = (lv_opa_t)(190.0f * f);
    draw_radial(layer, &d, ui.angle, (float)NEEDLE_FROM, (float)NEEDLE_TO);
}

static void draw_idle_ring(lv_layer_t *layer)
{
    if (ui.has_beam || ui.notice) {
        return;
    }
    draw_arc(layer, CX, CY, (int32_t)ui.ring_radius, 2, 0.0f, 359.9f, lv_color_hex(COL_TICK), LV_OPA_COVER, false);
}

static void field_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    draw_scale(layer);
    draw_heat(layer);
    draw_levels(layer);
    draw_idle_ring(layer);
    draw_beam(layer);
}

static void field_press_cb(lv_event_t *e)
{
    (void)e;
    if (ui.on_long_press) {
        ui.on_long_press();
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int spacing)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(l, spacing, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

void ui_init(lv_obj_t *screen, ui_long_press_cb_t on_long_press)
{
    memset(&ui, 0, sizeof(ui));
    ui.on_long_press = on_long_press;
    ui.shown_degrees = -1;
    ui.ring_radius = RING_MIN;
    ui.spread = 16.0f;

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    ui.field = lv_obj_create(screen);
    lv_obj_remove_style_all(ui.field);
    lv_obj_set_size(ui.field, SIZE, SIZE);
    lv_obj_set_pos(ui.field, 0, 0);
    lv_obj_remove_flag(ui.field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui.field, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui.field, field_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(ui.field, field_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    ui.digits = make_label(screen, &font_digits_124, COL_CENTRE, -2);
    ui.degree = make_label(screen, &font_digits_124, COL_TEXT_DIM, 0);
    ui.word = make_label(screen, &font_word_28, COL_CENTRE, 7);
    ui.detail = make_label(screen, &font_small_21, COL_TEXT_DIM, 1);
    ui.status = make_label(screen, &font_small_21, COL_TEXT_DIM, 4);
    ui.title = make_label(screen, &font_word_28, COL_CENTRE, 7);
    ui.subtitle = make_label(screen, &font_small_21, COL_TEXT_DIM, 1);
    ui.db[0] = make_label(screen, &font_small_21, COL_TEXT_DIM, 0);
    ui.db[1] = make_label(screen, &font_small_21, COL_TEXT_DIM, 0);

    lv_label_set_text(ui.degree, "\xC2\xB0");
    lv_obj_add_flag(ui.degree, LV_OBJ_FLAG_HIDDEN);

    lv_obj_align(ui.status, LV_ALIGN_TOP_MID, 0, 106);
    lv_obj_align(ui.word, LV_ALIGN_TOP_MID, 0, 296);
    lv_obj_align(ui.detail, LV_ALIGN_TOP_MID, 0, 335);
    lv_obj_align(ui.title, LV_ALIGN_CENTER, 0, -18);
    lv_obj_align(ui.subtitle, LV_ALIGN_CENTER, 0, 20);
    lv_obj_align(ui.db[0], LV_ALIGN_TOP_LEFT, 94, 330);
    lv_obj_align(ui.db[1], LV_ALIGN_TOP_RIGHT, -94, 330);
}

void ui_set_notice(const char *title, const char *detail)
{
    ui.notice = title != NULL;
    ui.idle_shown = false;
    if (ui.notice) {
        lv_label_set_text(ui.title, title);
        lv_label_set_text(ui.subtitle, detail ? detail : "");
        lv_obj_remove_flag(ui.title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui.subtitle, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_tracking_text_visible(bool visible)
{
    lv_obj_t *items[] = {ui.digits, ui.degree, ui.word, ui.detail, ui.status};
    for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (visible) {
            lv_obj_remove_flag(items[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(items[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_readout(const ui_state_t *s)
{
    int degrees = (int)lroundf(fabsf(ui.angle));
    lv_color_t color = side_color(ui.angle);

    if (degrees != ui.shown_degrees) {
        ui.shown_degrees = degrees;
        char text[8];
        snprintf(text, sizeof(text), "%d", degrees);
        lv_label_set_text(ui.digits, text);
        lv_obj_align(ui.digits, LV_ALIGN_CENTER, 0, -24);
        lv_obj_update_layout(ui.digits);
        lv_obj_align_to(ui.degree, ui.digits, LV_ALIGN_OUT_RIGHT_TOP, 2, 0);
    }

    const char *word = degrees < 7 ? "AHEAD" : (ui.angle < 0 ? "LEFT" : "RIGHT");
    if (strcmp(lv_label_get_text(ui.word), word) != 0) {
        lv_label_set_text(ui.word, word);
    }
    lv_obj_set_style_text_color(ui.word, color, 0);

    char detail[40];
    int gap = abs((int)lroundf(s->tau_us));
    if (degrees < 3) {
        snprintf(detail, sizeof(detail), "both mics together");
    } else {
        snprintf(detail, sizeof(detail), "%s mic first \xC2\xB7 %d \xC2\xB5s", ui.angle < 0 ? "left" : "right", gap);
    }
    lv_label_set_text(ui.detail, detail);
    lv_label_set_text(ui.status, s->live ? "HEARING" : "LAST HEARD");

    lv_opa_t opa = (lv_opa_t)(60.0f + 195.0f * ui.fade);
    lv_obj_set_style_text_opa(ui.digits, opa, 0);
    lv_obj_set_style_text_opa(ui.degree, opa, 0);
    lv_obj_set_style_text_opa(ui.word, opa, 0);
}

static void update_heat(const ui_state_t *s, float dt)
{
    if (s->live && s->tracking) {
        float pos = (s->angle_deg + 90.0f) / 5.0f;
        int centre = (int)lroundf(pos);
        for (int i = centre - 1; i <= centre + 1; i++) {
            if (i < 0 || i >= HEAT_BINS) {
                continue;
            }
            float weight = i == centre ? 0.11f : 0.04f;
            ui.heat[i] = clampf(ui.heat[i] + weight * s->confidence, 0.0f, 1.0f);
            lv_area_t a;
            float b = -90.0f + 5.0f * (float)i;
            sector_area(b - 2.0f, b + 2.0f, (float)(HEAT_BASE - HEAT_LENGTH), (float)HEAT_BASE, &a);
            lv_obj_invalidate_area(ui.field, &a);
        }
    }

    // Decay in steps, because each step redraws the whole band.
    ui.decay_timer += dt;
    if (ui.decay_timer >= 0.16f) {
        bool any = false;
        float k = powf(0.5f, ui.decay_timer / 2.2f);
        for (int i = 0; i < HEAT_BINS; i++) {
            if (ui.heat[i] > 0.0f) {
                ui.heat[i] *= k;
                if (ui.heat[i] < 0.02f) {
                    ui.heat[i] = 0.0f;
                }
                any = true;
            }
        }
        ui.decay_timer = 0.0f;
        if (any) {
            lv_area_t band = {CX - HEAT_BASE - 4, CY - HEAT_BASE - 4, CX + HEAT_BASE + 4, CY + 8};
            lv_obj_invalidate_area(ui.field, &band);
        }
    }
}

static void update_levels(const ui_state_t *s, float dt)
{
    bool changed = false;
    for (int m = 0; m < 2; m++) {
        float target = clampf((s->level_db[m] + 72.0f) / 54.0f, 0.0f, 1.0f);
        float rate = target > ui.level[m] ? 26.0f : 5.0f;
        float next = ui.level[m] + (1.0f - expf(-rate * dt)) * (target - ui.level[m]);
        changed |= fabsf(next - ui.level[m]) > 0.004f;
        ui.level[m] = next;
    }
    if (changed) {
        lv_area_t left = {38, CY + 30, CX - 18, SIZE - 30};
        lv_area_t right = {CX + 18, CY + 30, SIZE - 38, SIZE - 30};
        lv_obj_invalidate_area(ui.field, &left);
        lv_obj_invalidate_area(ui.field, &right);
    }

    ui.db_timer += dt;
    if (ui.db_timer >= 0.25f) {
        ui.db_timer = 0.0f;
        for (int m = 0; m < 2; m++) {
            char text[16];
            int db = (int)lroundf(fabsf(s->level_db[m]));
            snprintf(text, sizeof(text), "\xE2\x88\x92%d dB", db);
            lv_label_set_text(ui.db[m], text);
        }
    }
}

static void update_idle(float dt)
{
    if (!ui.idle_shown) {
        ui.idle_shown = true;
        lv_label_set_text(ui.title, "LISTENING");
        lv_label_set_text(ui.subtitle, "clap or talk nearby");
        lv_obj_remove_flag(ui.title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui.subtitle, LV_OBJ_FLAG_HIDDEN);
    }
    ui.breath += dt * 0.9f;
    if (ui.breath > PI2) {
        ui.breath -= PI2;
    }
    float loud = fmaxf(ui.level[0], ui.level[1]);
    float r = RING_MIN + 5.0f * sinf(ui.breath) + (RING_MAX - RING_MIN) * loud;
    if (fabsf(r - ui.ring_radius) >= 1.0f) {
        lv_obj_invalidate_area(ui.field, &ui.ring_area);
        ui.ring_radius = r;
        int32_t e = (int32_t)r + 3;
        lv_area_t a = {CX - e, CY - e, CX + e, CY + e};
        ui.ring_area = a;
        lv_obj_invalidate_area(ui.field, &a);
    }
}

void ui_update(const ui_state_t *s, float dt)
{
    update_levels(s, dt);
    update_heat(s, dt);

    bool show_beam = s->tracking && !ui.notice;
    if (show_beam != ui.has_beam) {
        if (show_beam) {
            ui.angle = s->angle_deg;
            ui.idle_shown = false;
            lv_obj_add_flag(ui.title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui.subtitle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate_area(ui.field, &ui.ring_area);
            ui.shown_degrees = -1;
        }
        set_tracking_text_visible(show_beam);
        ui.has_beam = show_beam;
        lv_obj_invalidate(ui.field);
    }

    if (ui.has_beam) {
        lv_obj_invalidate_area(ui.field, &ui.beam_area);
        lv_obj_invalidate_area(ui.field, &ui.ghost_area);

        ui.angle += (1.0f - expf(-16.0f * dt)) * (s->angle_deg - ui.angle);
        float target = 4.0f + (1.0f - clampf(s->confidence, 0.0f, 1.0f)) * 24.0f;
        ui.spread += (1.0f - expf(-8.0f * dt)) * (target - ui.spread);
        ui.fade = clampf(1.0f - (s->age_s - 0.25f) / 2.25f, 0.0f, 1.0f);

        float m = ui.spread + 4.0f;
        sector_area(ui.angle - m, ui.angle + m, (float)(NEEDLE_FROM - 2), (float)(BEAM_OUTER + 12), &ui.beam_area);
        sector_area(180.0f - ui.angle - m, 180.0f - ui.angle + m, (float)(BEAM_OUTER - 8), (float)(BEAM_OUTER + 2), &ui.ghost_area);
        lv_obj_invalidate_area(ui.field, &ui.beam_area);
        lv_obj_invalidate_area(ui.field, &ui.ghost_area);

        ui.text_timer += dt;
        if (ui.text_timer >= 0.1f) {
            ui.text_timer = 0.0f;
            update_readout(s);
        }
    } else if (!ui.notice) {
        update_idle(dt);
    }
}
