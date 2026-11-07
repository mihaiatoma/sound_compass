// Host test for main/turn.c and doa_clap_turn. A picture is cut into rectangles the
// way the display code sends them, sent in pieces to a stand-in panel, and compared
// with the whole picture turned by its coordinates.
//
//   cc -O2 -std=c11 -Imain test/test_turn.c main/turn.c main/doa.c -lm -o build_host/test_turn && build_host/test_turn
#include <stdio.h>
#include <string.h>

#include "doa.h"
#include "turn.h"

#define SIZE 466

static uint16_t s_pic[SIZE * SIZE], s_panel[SIZE * SIZE];
static int s_fail;

static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        s_fail++;
    }
}

static uint16_t pixel(int x, int y)
{
    return (uint16_t)(x * 3 + y * 1021 + 1);
}

// Where a pixel of the picture lands when turned clockwise.
static void landing(int quarters, int x, int y, int *px, int *py)
{
    int ox = x, oy = y;
    for (int i = 0; i < quarters; i++) {
        int t = ox;
        ox = SIZE - 1 - oy;
        oy = t;
    }
    *px = ox;
    *py = oy;
}

// Stands in for the panel: paints each piece and checks the rules a piece must keep.
static uint16_t s_strip_a[SIZE * 8], s_strip_b[SIZE * 8];
static turn_strips_t s_strips = {{s_strip_a, s_strip_b}, SIZE * 8, 0};
static const uint16_t *s_last_buffer;
static int s_pieces_ok;

static int panel_send(const turn_rect_t *r, const uint16_t *pixels, void *ctx)
{
    (void)ctx;
    int w = r->x2 - r->x1, h = r->y2 - r->y1, i = 0;
    for (int y = r->y1; y < r->y2; y++) {
        for (int x = r->x1; x < r->x2; x++) {
            s_panel[y * SIZE + x] = pixels[i++];
        }
    }
    s_pieces_ok &= r->x1 >= 0 && r->y1 >= 0 && r->x2 <= SIZE && r->y2 <= SIZE;
    s_pieces_ok &= r->x1 % 2 == 0 && r->y1 % 2 == 0 && r->x2 % 2 == 0 && r->y2 % 2 == 0;
    s_pieces_ok &= w * h <= s_strips.capacity;
    s_pieces_ok &= pixels != s_last_buffer;      // the one before may still be sending
    s_last_buffer = pixels;
    return 0;
}

static void send(int quarters, int x1, int y1, int x2, int y2)
{
    static uint16_t src[SIZE * 50];
    int i = 0;
    for (int y = y1; y < y2; y++) {
        for (int x = x1; x < x2; x++) {
            src[i++] = s_pic[y * SIZE + x];
        }
    }
    turn_rect_t r = {x1, y1, x2, y2};
    int err = turn_send_strips(&s_strips, quarters, SIZE, r, src, panel_send, NULL);
    s_pieces_ok &= err == 0;
}

static void check_turns(void)
{
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            s_pic[y * SIZE + x] = pixel(x, y);
        }
    }

    for (int q = 0; q <= 3; q++) {
        memset(s_panel, 0, sizeof(s_panel));
        s_pieces_ok = 1;
        // Bands 50 rows tall, like the draw buffer, with uneven widths inside each.
        for (int y = 0; y < SIZE; y += 50) {
            int y2 = y + 50 > SIZE ? SIZE : y + 50;
            if (y == 200) {
                send(q, 0, y, SIZE, y2);        // one full band, the largest update
                continue;
            }
            send(q, 0, y, 148, y2);
            send(q, 148, y, 150, y2);
            send(q, 150, y, SIZE, y2);
        }
        int same = 1;
        for (int y = 0; y < SIZE; y++) {
            for (int x = 0; x < SIZE; x++) {
                int px, py;
                landing(q, x, y, &px, &py);
                same &= s_panel[py * SIZE + px] == pixel(x, y);
            }
        }
        char what[64];
        snprintf(what, sizeof(what), "%d quarter turns: every pixel lands right", q);
        check(same, what);
        snprintf(what, sizeof(what), "%d quarter turns: pieces stay valid", q);
        check(s_pieces_ok, what);
    }
}

// A tall narrow update, the shape of the bearing arc at the side of the screen.
static void check_tall_update(void)
{
    memset(s_panel, 0, sizeof(s_panel));
    s_pieces_ok = 1;
    send(1, 20, 60, 50, 360);
    int same = 1;
    for (int y = 60; y < 360; y++) {
        for (int x = 20; x < 50; x++) {
            int px, py;
            landing(1, x, y, &px, &py);
            same &= s_panel[py * SIZE + px] == pixel(x, y);
        }
    }
    check(same && s_pieces_ok, "tall narrow update lands right");
}

static int fail_second(const turn_rect_t *r, const uint16_t *pixels, void *ctx)
{
    (void)r;
    (void)pixels;
    int *calls = ctx;
    return ++*calls == 2 ? 7 : 0;
}

static void check_failure_stops(void)
{
    static uint16_t src[SIZE * 50];
    turn_rect_t r = {0, 0, SIZE, 50};
    int calls = 0;
    int err = turn_send_strips(&s_strips, 1, SIZE, r, src, fail_second, &calls);
    check(err == 7 && calls == 2, "a failed piece stops the update and reports it");
}

// A quarter turn clockwise must carry the top of the picture to the right of the panel.
static void check_direction(void)
{
    turn_rect_t r = {232, 0, 234, 2};
    uint16_t src[4] = {1, 2, 3, 4}, dst[4];
    turn_rect(1, SIZE, &r, src, dst);
    check(r.x1 == SIZE - 2 && r.y1 == 232, "clockwise carries the top to the right");
    r = (turn_rect_t){232, 0, 234, 2};
    turn_rect(3, SIZE, &r, src, dst);
    check(r.x1 == 0 && r.y1 == SIZE - 234, "anticlockwise carries the top to the left");
}

static void check_clap_turn(void)
{
    doa_config_t cfg = doa_default_config();
    doa_init(&cfg);
    check(doa_clap_turn(0.0f) == 0, "clap dead ahead keeps the screen");
    check(doa_clap_turn(-15.0f) == 0, "clap 13 degrees off keeps the screen");
    check(doa_clap_turn(40.0f) == 0, "clap 37 degrees off keeps the screen");
    check(doa_clap_turn(60.0f) == -1, "clap from the MIC2 end turns anticlockwise");
    check(doa_clap_turn(-60.0f) == 1, "clap from the MIC1 end turns clockwise");
    check(doa_clap_turn(70.0f) == -1, "delay past the end still counts");
}

int main(void)
{
    check_turns();
    check_tall_update();
    check_failure_stops();
    check_direction();
    check_clap_turn();
    printf("\n%s (%d failed)\n", s_fail ? "FAILED" : "ALL PASSED", s_fail);
    return s_fail ? 1 : 0;
}
