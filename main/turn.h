// Turns the picture on a square screen a quarter at a time, and sends it to the
// panel in small pieces. Plain C, so the host test (test/test_turn.c) runs this file.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x1, y1;   // first pixel
    int x2, y2;   // one past the last pixel
} turn_rect_t;

// Sends one piece to the panel. Returns 0 on success.
typedef int (*turn_send_t)(const turn_rect_t *rect, const uint16_t *pixels, void *ctx);

typedef struct {
    uint16_t *buffer[2];   // two, because the last piece may still be sending
    int capacity;          // pixels in each buffer, at least two screen rows
    int next;              // the buffer to fill next
} turn_strips_t;

// Copies one rectangle to where it lands when the picture is turned clockwise by
// quarters (0..3), and updates rect. dst must hold as many pixels as src.
void turn_rect(int quarters, int size, turn_rect_t *rect, const uint16_t *src, uint16_t *dst);

// Turns a rectangle and sends it as pieces that fit the buffers, cut on whole rows.
// The panel needs even boundaries, so a rectangle that starts on an even row keeps
// them. Stops at the first failure and returns it.
int turn_send_strips(turn_strips_t *strips, int quarters, int size, turn_rect_t rect,
                     const uint16_t *src, turn_send_t send, void *ctx);

#ifdef __cplusplus
}
#endif
