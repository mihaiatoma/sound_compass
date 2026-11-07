// The round display. Depends only on LVGL, so tools/preview can render this exact
// code to images on a computer without the board.
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool  tracking;       // a bearing is being held
    bool  live;           // the newest audio frame carried a usable sound
    float angle_deg;      // -90 (left) .. 0 (ahead) .. +90 (right)
    float confidence;     // 0..1
    float age_s;          // seconds since the last usable frame
    float tau_us;         // arrival gap, positive when the right mic hears first
    float level_db[2];    // left and right mic level in dBFS
} ui_state_t;

typedef void (*ui_long_press_cb_t)(void);

void ui_init(lv_obj_t *screen, ui_long_press_cb_t on_long_press);

// Call about 30 times a second, holding the display lock.
void ui_update(const ui_state_t *state, float dt_s);

// Show a message over the centre, or pass NULL to clear it.
void ui_set_notice(const char *title, const char *detail);

#ifdef __cplusplus
}
#endif
