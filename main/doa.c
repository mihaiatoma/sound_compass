// Two-microphone direction finding with noise-gated GCC-PHAT.
//
// A sound off to one side reaches one mic first. That delay shows up as a phase
// slope across frequency in the cross-spectrum of the two channels. The delay is
// read from phase and searched on a grid finer than one sample, so accuracy is
// not tied to the sample period.
//
//   tau = t1 - t2 = spacing * sin(angle) / c
//
// tau is positive when MIC2 hears the sound first, which is a positive angle.
#include "doa.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979f

// Delay candidates run a little past +/- tau_max so a real source at the end of
// the axis still forms a proper peak.
#define TAU_POINTS  81
#define TAU_MARGIN  1.15f

// Frame gates. A frame counts only when enough bins carry signal and the delay
// peak stands well above the average score. Phase agreement alone is not enough:
// low frequencies agree with every delay, because a 22.7 mm gap barely moves
// their phase, so they would look certain while saying nothing about direction.
#define MIN_ACTIVE_WEIGHT   20.0f   // total bin weight needed to trust a frame
#define MIN_CONFIDENCE      0.40f   // measured sharpness / best possible sharpness
#define BIN_GATE            4.5f    // bin must clear the noise floor by this factor

// Noise tracker per bin. It falls quickly and rises slowly, so it follows steady
// sounds such as fans and electrical whine but not claps or speech.
#define NOISE_FALL  0.95f
#define NOISE_RISE  1.01f

// Tracker.
#define TRACK_HOLD_S    2.5f
#define TRACK_ALPHA_MIN 0.08f
#define TRACK_ALPHA_MAX 0.45f

#define WINDOW_SECONDS ((float)DOA_HOP / (float)DOA_SAMPLE_RATE_HZ)

static doa_config_t s_cfg;
static float s_tau_max_s;
static float s_zero_offset_s;
static int   s_bin_lo, s_bin_hi;

static float s_hann[DOA_FFT_N];
static float s_tw_re[DOA_FFT_N / 2], s_tw_im[DOA_FFT_N / 2];
static uint16_t s_bitrev[DOA_FFT_N];
static float s_re[DOA_FFT_N], s_im[DOA_FFT_N];
static float s_noise[DOA_FFT_N / 2 + 1];
static bool  s_noise_ready;

// Noise-weighted, unit-magnitude cross-spectrum for the active band.
static float s_cross_re[DOA_FFT_N / 2 + 1], s_cross_im[DOA_FFT_N / 2 + 1];

// Per-delay rotation, computed once. A delay's phase term is advanced from bin
// to bin by complex multiplication, which needs no lookup table per bin.
static float s_step_re[TAU_POINTS], s_step_im[TAU_POINTS];
static float s_start_re[TAU_POINTS], s_start_im[TAU_POINTS];
static float s_tau_grid_s[TAU_POINTS];

// Sharpness (peak minus mean score) a perfect source reaches at each delay. The
// score curve sits higher on average near the centre, so raw sharpness would
// reject sounds from straight ahead more readily than sounds from the sides.
static float s_ideal_sharpness[TAU_POINTS];

doa_config_t doa_default_config(void)
{
    doa_config_t cfg = {
        .mic_spacing_m  = 0.0227f,   // MIC1 to MIC2 body centres in the STEP model
        .speed_of_sound = 343.0f,    // 20 C
        .band_lo_hz     = 500.0f,    // below this the phase shift is too small to read
        .band_hi_hz     = 7200.0f,   // spatial aliasing starts near 7.55 kHz
    };
    return cfg;
}

static void fft_init(void)
{
    for (int i = 0; i < DOA_FFT_N / 2; i++) {
        float a = -2.0f * PI_F * (float)i / (float)DOA_FFT_N;
        s_tw_re[i] = cosf(a);
        s_tw_im[i] = sinf(a);
    }
    int bits = 0;
    while ((1 << bits) < DOA_FFT_N) {
        bits++;
    }
    for (int i = 0; i < DOA_FFT_N; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) {
            if (i & (1 << b)) {
                r |= 1 << (bits - 1 - b);
            }
        }
        s_bitrev[i] = (uint16_t)r;
    }
}

static void fft(float *re, float *im)
{
    for (int i = 0; i < DOA_FFT_N; i++) {
        int j = s_bitrev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= DOA_FFT_N; len <<= 1) {
        int half = len / 2;
        int step = DOA_FFT_N / len;
        for (int i = 0; i < DOA_FFT_N; i += len) {
            for (int k = 0; k < half; k++) {
                int a = i + k, b = a + half;
                float wr = s_tw_re[k * step], wi = s_tw_im[k * step];
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

void doa_init(const doa_config_t *config)
{
    s_cfg = *config;
    s_tau_max_s = s_cfg.mic_spacing_m / s_cfg.speed_of_sound;
    s_zero_offset_s = 0.0f;
    s_noise_ready = false;

    fft_init();
    for (int i = 0; i < DOA_FFT_N; i++) {
        s_hann[i] = 0.5f - 0.5f * cosf(2.0f * PI_F * (float)i / (float)(DOA_FFT_N - 1));
    }

    float bin_hz = (float)DOA_SAMPLE_RATE_HZ / (float)DOA_FFT_N;
    s_bin_lo = (int)ceilf(s_cfg.band_lo_hz / bin_hz);
    s_bin_hi = (int)floorf(s_cfg.band_hi_hz / bin_hz);

    for (int j = 0; j < TAU_POINTS; j++) {
        float u = -1.0f + 2.0f * (float)j / (float)(TAU_POINTS - 1);
        float tau = u * TAU_MARGIN * s_tau_max_s;
        s_tau_grid_s[j] = tau;
        float w = 2.0f * PI_F * bin_hz * tau;
        s_step_re[j] = cosf(w);
        s_step_im[j] = sinf(w);
        s_start_re[j] = cosf(w * (float)s_bin_lo);
        s_start_im[j] = sinf(w * (float)s_bin_lo);
    }

    float kernel[TAU_POINTS];
    float grid = s_tau_grid_s[1] - s_tau_grid_s[0];
    for (int d = 0; d < TAU_POINTS; d++) {
        float sum = 0.0f;
        for (int k = s_bin_lo; k <= s_bin_hi; k++) {
            sum += cosf(2.0f * PI_F * bin_hz * (float)k * grid * (float)d);
        }
        kernel[d] = sum / (float)(s_bin_hi - s_bin_lo + 1);
    }
    for (int j = 0; j < TAU_POINTS; j++) {
        float sum = 0.0f;
        for (int i = 0; i < TAU_POINTS; i++) {
            sum += kernel[i > j ? i - j : j - i];
        }
        s_ideal_sharpness[j] = 1.0f - sum / (float)TAU_POINTS;
    }
}

void doa_set_zero_offset_us(float tau_us)
{
    s_zero_offset_s = tau_us * 1e-6f;
}

static float level_db(float sum_squares)
{
    float rms2 = sum_squares / (float)DOA_FFT_N;
    return 10.0f * log10f(rms2 + 1e-12f);
}

void doa_process(const float *mic1, const float *mic2, doa_frame_t *out)
{
    float e1 = 0.0f, e2 = 0.0f, mean1 = 0.0f, mean2 = 0.0f;
    for (int i = 0; i < DOA_FFT_N; i++) {
        mean1 += mic1[i];
        mean2 += mic2[i];
    }
    mean1 /= (float)DOA_FFT_N;
    mean2 /= (float)DOA_FFT_N;

    // One complex FFT carries both real channels: z = mic1 + j mic2.
    for (int i = 0; i < DOA_FFT_N; i++) {
        float a = mic1[i] - mean1, b = mic2[i] - mean2;
        e1 += a * a;
        e2 += b * b;
        s_re[i] = a * s_hann[i];
        s_im[i] = b * s_hann[i];
    }
    fft(s_re, s_im);

    out->level_db_mic1 = level_db(e1);
    out->level_db_mic2 = level_db(e2);
    out->sound = false;
    out->angle_deg = 0.0f;
    out->tau_us = 0.0f;
    out->confidence = 0.0f;

    float total_weight = 0.0f;
    for (int k = s_bin_lo; k <= s_bin_hi; k++) {
        int m = DOA_FFT_N - k;
        // Split the two spectra using conjugate symmetry.
        float x1r = 0.5f * (s_re[k] + s_re[m]);
        float x1i = 0.5f * (s_im[k] - s_im[m]);
        float x2r = 0.5f * (s_im[k] + s_im[m]);
        float x2i = -0.5f * (s_re[k] - s_re[m]);

        // X1 * conj(X2), whose phase is -w * (t1 - t2).
        float cr = x1r * x2r + x1i * x2i;
        float ci = x1i * x2r - x1r * x2i;
        float mag = sqrtf(cr * cr + ci * ci) + 1e-12f;

        if (!s_noise_ready) {
            s_noise[k] = mag;
        } else if (mag < s_noise[k]) {
            s_noise[k] = NOISE_FALL * s_noise[k] + (1.0f - NOISE_FALL) * mag;
        } else {
            s_noise[k] *= NOISE_RISE;
        }

        // Bins near the noise floor get no vote. PHAT keeps phase only, so a
        // quiet bin would otherwise count as much as a loud one.
        float gate = 1.0f - BIN_GATE * s_noise[k] / mag;
        float w = gate > 0.0f ? gate : 0.0f;
        s_cross_re[k] = w * cr / mag;
        s_cross_im[k] = w * ci / mag;
        total_weight += w;
    }
    s_noise_ready = true;

    if (total_weight < MIN_ACTIVE_WEIGHT) {
        return;
    }

    // Score every candidate delay by how well it flattens the phase slope.
    float best = -1e30f;
    float score_sum = 0.0f;
    int best_j = 0;
    float scores[TAU_POINTS];
    for (int j = 0; j < TAU_POINTS; j++) {
        float er = s_start_re[j], ei = s_start_im[j];
        const float sr = s_step_re[j], si = s_step_im[j];
        float score = 0.0f;
        for (int k = s_bin_lo; k <= s_bin_hi; k++) {
            score += s_cross_re[k] * er - s_cross_im[k] * ei;
            float t = er * sr - ei * si;
            ei = er * si + ei * sr;
            er = t;
        }
        scores[j] = score;
        score_sum += score;
        if (score > best) {
            best = score;
            best_j = j;
        }
    }

    // Parabolic fit around the winner.
    float tau = s_tau_grid_s[best_j];
    if (best_j > 0 && best_j < TAU_POINTS - 1) {
        float a = scores[best_j - 1], b = scores[best_j], c = scores[best_j + 1];
        float denom = a - 2.0f * b + c;
        if (denom < -1e-6f) {
            float shift = 0.5f * (a - c) / denom;
            float grid = s_tau_grid_s[1] - s_tau_grid_s[0];
            tau += shift * grid;
        }
    }
    tau -= s_zero_offset_s;

    float sharpness = (best - score_sum / (float)TAU_POINTS) / total_weight;
    float confidence = sharpness / s_ideal_sharpness[best_j];
    if (confidence < MIN_CONFIDENCE) {
        return;
    }

    float s = tau / s_tau_max_s;
    if (s > 1.0f) {
        s = 1.0f;
    } else if (s < -1.0f) {
        s = -1.0f;
    }
    out->sound = true;
    out->tau_us = tau * 1e6f;
    out->angle_deg = asinf(s) * 180.0f / PI_F;
    out->confidence = confidence > 1.0f ? 1.0f : confidence;
}

void doa_track_reset(doa_track_t *track)
{
    memset(track, 0, sizeof(*track));
}

static float median_recent(const doa_track_t *track)
{
    float v[DOA_TRACK_MEDIAN];
    int n = track->recent_count;
    for (int i = 0; i < n; i++) {
        float x = track->recent[i];
        int j = i;
        for (; j > 0 && v[j - 1] > x; j--) {
            v[j] = v[j - 1];
        }
        v[j] = x;
    }
    return v[n / 2];
}

void doa_track_update(doa_track_t *track, const doa_frame_t *frame)
{
    if (!frame->sound) {
        track->age_s += WINDOW_SECONDS;
        if (track->age_s > TRACK_HOLD_S) {
            track->valid = false;
            track->recent_count = 0;
        }
        return;
    }

    track->recent[track->recent_next] = frame->angle_deg;
    track->recent_next = (track->recent_next + 1) % DOA_TRACK_MEDIAN;
    if (track->recent_count < DOA_TRACK_MEDIAN) {
        track->recent_count++;
    }
    float angle = median_recent(track);

    if (!track->valid) {
        track->angle_deg = angle;
        track->confidence = frame->confidence;
        track->valid = true;
    } else {
        float alpha = TRACK_ALPHA_MIN + (TRACK_ALPHA_MAX - TRACK_ALPHA_MIN) * frame->confidence;
        track->angle_deg += alpha * (angle - track->angle_deg);
        track->confidence += 0.2f * (frame->confidence - track->confidence);
    }
    track->age_s = 0.0f;
}
