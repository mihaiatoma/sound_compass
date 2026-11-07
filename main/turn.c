#include "turn.h"

#include <string.h>

void turn_rect(int quarters, int size, turn_rect_t *rect, const uint16_t *src, uint16_t *dst)
{
    const turn_rect_t in = *rect;
    const int w = in.x2 - in.x1, h = in.y2 - in.y1;

    switch (quarters) {
    case 1:    // (x, y) lands on (size - 1 - y, x)
        rect->x1 = size - in.y2;
        rect->x2 = size - in.y1;
        rect->y1 = in.x1;
        rect->y2 = in.x2;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                dst[x * h + (h - 1 - y)] = *src++;
            }
        }
        break;
    case 2:    // (x, y) lands on (size - 1 - x, size - 1 - y)
        rect->x1 = size - in.x2;
        rect->x2 = size - in.x1;
        rect->y1 = size - in.y2;
        rect->y2 = size - in.y1;
        for (int i = w * h - 1; i >= 0; i--) {
            dst[i] = *src++;
        }
        break;
    case 3:    // (x, y) lands on (y, size - 1 - x)
        rect->x1 = in.y1;
        rect->x2 = in.y2;
        rect->y1 = size - in.x2;
        rect->y2 = size - in.x1;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                dst[(w - 1 - x) * h + y] = *src++;
            }
        }
        break;
    default:   // (x, y) stays where it is
        memcpy(dst, src, (size_t)(w * h) * sizeof(uint16_t));
        break;
    }
}

int turn_send_strips(turn_strips_t *strips, int quarters, int size, turn_rect_t rect,
                     const uint16_t *src, turn_send_t send, void *ctx)
{
    const int w = rect.x2 - rect.x1;
    const int rows = (strips->capacity / w) & ~1;

    for (int y = rect.y1; y < rect.y2; y += rows) {
        const int end = y + rows < rect.y2 ? y + rows : rect.y2;
        turn_rect_t piece = {rect.x1, y, rect.x2, end};
        uint16_t *dst = strips->buffer[strips->next];
        strips->next ^= 1;

        turn_rect(quarters, size, &piece, src, dst);
        int err = send(&piece, dst, ctx);
        if (err != 0) {
            return err;
        }
        src += w * (end - y);
    }
    return 0;
}
