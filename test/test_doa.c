// Host test for main/doa.c. It simulates the board: two mics 22.7 mm apart at
// 24 kHz, 16-bit, with sound arriving from known angles.
//
//   cc -O2 -std=c11 -Imain test/test_doa.c main/doa.c -lm -o build_host/test_doa && build_host/test_doa
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "doa.h"

#define PI 3.14159265358979
#define LEN (1 << 16)          // 2.7 seconds of signal
#define SPACING 0.0227
#define C 343.0
#define LEAD_IN 9600         // 0.4 s of room noise before the sound starts

static double s_src[LEN], s_a[LEN], s_b[LEN];
static float s_w1[DOA_FFT_N], s_w2[DOA_FFT_N];

static unsigned s_rng = 12345;
static double uni(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (double)(s_rng >> 8) / (double)(1u << 24);
}
static double gauss(void)
{
    double u = uni() + 1e-12, v = uni();
    return sqrt(-2.0 * log(u)) * cos(2.0 * PI * v);
}

// Radix-2 FFT in double, separate from the code under test.
static void dfft(double *re, double *im, int n, int inverse)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * PI / len * (inverse ? 1 : -1);
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; k++) {
                double wr = cos(ang * k), wi = sin(ang * k);
                int a = i + k, b = a + len / 2;
                double tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
            }
        }
    }
    if (inverse) {
        for (int i = 0; i < n; i++) {
            re[i] /= n;
            im[i] /= n;
        }
    }
}

// Exact fractional delay by a frequency-domain phase ramp.
static void delay_signal(const double *in, double *out, double delay_samples)
{
    static double re[LEN], im[LEN];
    memcpy(re, in, sizeof(re));
    memset(im, 0, sizeof(im));
    dfft(re, im, LEN, 0);
    for (int k = 0; k <= LEN / 2; k++) {
        double w = -2.0 * PI * k / LEN * delay_samples;
        double c = cos(w), s = sin(w);
        double r = re[k] * c - im[k] * s, i = re[k] * s + im[k] * c;
        re[k] = r; im[k] = i;
        if (k > 0 && k < LEN / 2) {
            re[LEN - k] = r;
            im[LEN - k] = -i;
        }
    }
    im[0] = 0;
    im[LEN / 2] = 0;
    dfft(re, im, LEN, 1);
    memcpy(out, re, sizeof(re));
}

enum { SRC_NOISE, SRC_SPEECHLIKE, SRC_CLAPS, SRC_TONE };

static void make_source(int kind)
{
    double lp1 = 0, lp2 = 0;
    for (int i = 0; i < LEN; i++) {
        double t = (double)i / DOA_SAMPLE_RATE_HZ, x = gauss();
        switch (kind) {
        case SRC_NOISE:
            s_src[i] = 0.1 * x;
            break;
        case SRC_SPEECHLIKE: {
            // Formant-like colouring with a 4 Hz syllable envelope.
            lp1 += 0.25 * (x - lp1);
            lp2 += 0.10 * (lp1 - lp2);
            double env = pow(0.5 + 0.5 * sin(2 * PI * 4 * t), 2.0);
            double voiced = sin(2 * PI * 140 * t) + 0.6 * sin(2 * PI * 280 * t) + 0.4 * sin(2 * PI * 420 * t);
            s_src[i] = env * (0.25 * lp2 + 0.05 * voiced + 0.03 * lp1);
            break;
        }
        case SRC_CLAPS: {
            double ph = fmod(t, 0.5);
            s_src[i] = 0.4 * exp(-ph * 90.0) * x;
            break;
        }
        case SRC_TONE:
            s_src[i] = 0.2 * sin(2 * PI * 1000 * t);
            break;
        }
    }
}

// Run one case. Returns mean signed error and fills spread and frames used.
typedef struct { double mean, worst, conf, tracked_abs; int used, total, tracked_n; } result_t;

// Early reflections as seen in a small room: delayed copies from other angles.
static const struct { double ms, gain, angle; } REFLECTIONS[] = {
    {3.1, 0.55, -35}, {5.7, 0.45, 70}, {9.3, 0.35, -75}, {14.0, 0.28, 20}, {21.0, 0.2, -10}, {33.0, 0.14, 55},
};

static void add_reverb(double angle_unused)
{
    (void)angle_unused;
    static double acc_a[LEN], acc_b[LEN], tmp[LEN];
    memcpy(acc_a, s_a, sizeof(acc_a));
    memcpy(acc_b, s_b, sizeof(acc_b));
    for (unsigned r = 0; r < sizeof(REFLECTIONS) / sizeof(REFLECTIONS[0]); r++) {
        double d = SPACING * sin(REFLECTIONS[r].angle * PI / 180.0) / C * DOA_SAMPLE_RATE_HZ;
        double lag = REFLECTIONS[r].ms * 1e-3 * DOA_SAMPLE_RATE_HZ;
        delay_signal(s_src, tmp, lag + 0.5 * d);
        for (int i = 0; i < LEN; i++) {
            acc_a[i] += REFLECTIONS[r].gain * tmp[i];
        }
        delay_signal(s_src, tmp, lag - 0.5 * d);
        for (int i = 0; i < LEN; i++) {
            acc_b[i] += REFLECTIONS[r].gain * tmp[i];
        }
    }
    memcpy(s_a, acc_a, sizeof(acc_a));
    memcpy(s_b, acc_b, sizeof(acc_b));
}

static int s_reverb;

static result_t run_case(double angle_deg, int kind, double noise_rms, double common_hum)
{
    double tau = SPACING * sin(angle_deg * PI / 180.0) / C;     // t1 - t2
    double delay = tau * DOA_SAMPLE_RATE_HZ;
    make_source(kind);
    delay_signal(s_src, s_a, +0.5 * delay);   // MIC1 later for positive angles
    delay_signal(s_src, s_b, -0.5 * delay);
    if (s_reverb) {
        add_reverb(angle_deg);
    }

    doa_config_t cfg = doa_default_config();
    doa_init(&cfg);
    doa_track_t track;
    doa_track_reset(&track);

    result_t r = {0};
    double sum = 0;
    for (int pos = 0; pos + DOA_FFT_N <= LEN; pos += DOA_HOP) {
        for (int i = 0; i < DOA_FFT_N; i++) {
            double hum = common_hum * sin(2 * PI * 3100.0 * (pos + i) / DOA_SAMPLE_RATE_HZ);
            // Start from a quiet room so the noise tracker learns the room,
            // as it does on the board, and not the test signal.
            double on = pos + i < LEAD_IN ? 0.0 : 1.0;
            double a = on * s_a[pos + i] + hum, b = on * s_b[pos + i] + hum;
            if (noise_rms > 0) {
                a += noise_rms * gauss();
                b += noise_rms * gauss();
            }
            // Round to 16 bits, as the ADC does.
            s_w1[i] = (float)(round(a * 32768.0) / 32768.0);
            s_w2[i] = (float)(round(b * 32768.0) / 32768.0);
        }
        doa_frame_t f;
        doa_process(s_w1, s_w2, &f);
        doa_track_update(&track, &f);
        r.total += pos > LEAD_IN;
        if (track.valid && pos > LEAD_IN) {
            r.tracked_abs += fabs(track.angle_deg - angle_deg);
            r.tracked_n++;
        }
        // Skip the warm-up frames where the noise tracker is settling.
        if (f.sound && pos > LEAD_IN) {
            double e = f.angle_deg - angle_deg;
            sum += e;
            if (fabs(e) > fabs(r.worst)) {
                r.worst = e;
            }
            r.conf += f.confidence;
            r.used++;
        }
    }
    if (r.used) {
        r.mean = sum / r.used;
        r.conf /= r.used;
    }
    if (r.tracked_n) {
        r.tracked_abs /= r.tracked_n;
    }
    return r;
}

static int s_fail;

static void check_sweep(const char *name, int kind, double noise, double hum, double max_track_err, double min_used)
{
    static const double angles[] = {-80, -60, -45, -30, -15, 0, 15, 30, 45, 60, 80};
    printf("\n%s  (noise rms %.4f, shared hum %.4f%s)\n", name, noise, hum, s_reverb ? ", room echoes" : "");
    printf("  true  frame_mean  frame_worst  conf  frames   bearing_err\n");
    double sum = 0;
    for (unsigned i = 0; i < sizeof(angles) / sizeof(angles[0]); i++) {
        result_t r = run_case(angles[i], kind, noise, hum);
        // sin() flattens near the mic axis, so a small timing error costs more there.
        double limit = fabs(angles[i]) >= 60 ? max_track_err * 2.0 : max_track_err;
        double used = r.total ? (double)r.used / r.total : 0;
        int ok = r.tracked_abs <= limit && used >= min_used;
        printf("  %+4.0f   %+7.2f     %+7.1f    %.2f  %3d/%3d   %5.2f %s\n", angles[i], r.mean, r.worst, r.conf, r.used, r.total, r.tracked_abs, ok ? "" : "FAIL");
        sum += r.tracked_abs;
        if (!ok) {
            s_fail++;
        }
    }
    printf("  average bearing error %.1f deg\n", sum / 11.0);
}

// A pure tone has one frequency, so its bearing is weak but it must not crash
// and its side must still be right at moderate angles.
static void check_tone(void)
{
    result_t r = run_case(40, SRC_TONE, 0.0005, 0);
    printf("\ntone 1 kHz at +40: %d of %d frames reported\n", r.used, r.total);
    if (r.used > r.total / 10) {
        printf("  FAIL: a single low tone carries no usable direction\n");
        s_fail++;
    }
}

// With nothing but room noise the tracker must not report a bearing.
static void check_silence(void)
{
    doa_config_t cfg = doa_default_config();
    doa_init(&cfg);
    int reported = 0, frames = 0;
    for (int n = 0; n < 400; n++) {
        for (int i = 0; i < DOA_FFT_N; i++) {
            s_w1[i] = (float)(0.0008 * gauss());
            s_w2[i] = (float)(0.0008 * gauss());
        }
        doa_frame_t f;
        doa_process(s_w1, s_w2, &f);
        if (n > 40) {
            frames++;
            reported += f.sound;
        }
    }
    printf("\nsilence: false bearings %d of %d frames\n", reported, frames);
    if (reported > frames / 20) {
        printf("  FAIL\n");
        s_fail++;
    }
}

// Sign check on its own, because a flipped sign swaps left and right.
static void check_sign(void)
{
    result_t right = run_case(+45, SRC_NOISE, 0.001, 0);
    result_t left = run_case(-45, SRC_NOISE, 0.001, 0);
    double rm = 45 + right.mean, lm = -45 + left.mean;
    printf("\nsign: MIC2 hears first -> %+.1f deg, MIC1 hears first -> %+.1f deg\n", rm, lm);
    if (rm <= 0 || lm >= 0) {
        printf("  FAIL\n");
        s_fail++;
    }
}

// Each loud frame follows a quiet one, so it runs the full delay search.
static void bench(void)
{
    doa_config_t cfg = doa_default_config();
    doa_init(&cfg);
    doa_frame_t f;
    double spent = 0;
    int n = 400;
    for (int it = 0; it < n; it++) {
        for (int i = 0; i < DOA_FFT_N; i++) {
            s_w1[i] = (float)(0.0005 * gauss());
            s_w2[i] = (float)(0.0005 * gauss());
        }
        doa_process(s_w1, s_w2, &f);
        for (int i = 0; i < DOA_FFT_N; i++) {
            double x = 0.2 * gauss();
            s_w1[i] = (float)x;
            s_w2[i] = (float)x;
        }
        clock_t t0 = clock();
        doa_process(s_w1, s_w2, &f);
        spent += (double)(clock() - t0);
    }
    printf("\nhost speed: %.0f us per full frame (reported: %d)\n", 1e6 * spent / CLOCKS_PER_SEC / n, f.sound);
}

int main(void)
{
    check_sign();
    check_sweep("white noise, high SNR", SRC_NOISE, 0.002, 0, 1.0, 0.8);
    check_sweep("speech-like, quiet room", SRC_SPEECHLIKE, 0.002, 0, 4.0, 0.25);
    // Here noise beats the speech above 1 kHz, so few frames qualify. The tracker
    // holds a bearing for 2.5 s, so a few usable frames per second are enough.
    check_sweep("speech-like, noisy room", SRC_SPEECHLIKE, 0.01, 0, 8.0, 0.03);
    check_sweep("claps", SRC_CLAPS, 0.002, 0, 6.0, 0.06);
    check_sweep("speech-like plus electrical whine in both mics", SRC_SPEECHLIKE, 0.002, 0.01, 6.0, 0.2);
    s_reverb = 1;
    check_sweep("speech-like in a reflective room", SRC_SPEECHLIKE, 0.002, 0, 15.0, 0.1);
    check_sweep("claps in a reflective room", SRC_CLAPS, 0.002, 0, 15.0, 0.04);   // about one frame per clap
    s_reverb = 0;
    check_tone();
    check_silence();
    bench();
    printf("\n%s (%d failed)\n", s_fail ? "FAILED" : "ALL PASSED", s_fail);
    return s_fail ? 1 : 0;
}
