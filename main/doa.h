// Direction-of-arrival from two microphones.
//
// Plain C with no ESP-IDF dependencies, so the same file runs on the board and
// in the host test (test/test_doa.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOA_SAMPLE_RATE_HZ 24000
#define DOA_FFT_N          1024                // 42.7 ms analysis window
#define DOA_HOP            (DOA_FFT_N / 2)     // a new estimate every 21.3 ms
#define DOA_TRACK_MEDIAN   5                   // frames, removes one-off outliers

typedef struct {
    float mic_spacing_m;   // distance between the two sound ports
    float speed_of_sound;  // m/s
    float band_lo_hz;      // ignore rumble below this
    float band_hi_hz;      // must stay below speed_of_sound / (2 * spacing)
} doa_config_t;

typedef struct {
    bool  sound;           // this frame held a usable directional sound
    float angle_deg;       // -90 (MIC1 side) .. 0 (ahead) .. +90 (MIC2 side)
    float tau_us;          // arrival-time difference, positive when MIC2 hears first
    float confidence;      // 0..1, 1 means one clean plane wave
    float level_db_mic1;   // dBFS
    float level_db_mic2;   // dBFS
} doa_frame_t;

typedef struct {
    bool  valid;           // a bearing is being held
    float angle_deg;       // smoothed bearing
    float confidence;      // smoothed 0..1
    float age_s;           // seconds since the last usable frame
    float recent[DOA_TRACK_MEDIAN];
    int   recent_count;
    int   recent_next;
} doa_track_t;

// Defaults for this board. Spacing comes from Waveshare's 3D model.
doa_config_t doa_default_config(void);

void doa_init(const doa_config_t *config);

// Analyse one window. Both inputs hold DOA_FFT_N samples in the range -1..1.
void doa_process(const float *mic1, const float *mic2, doa_frame_t *out);

// Feed each frame to the tracker, which holds the bearing between sounds.
void doa_track_reset(doa_track_t *track);
void doa_track_update(doa_track_t *track, const doa_frame_t *frame);

// Zero calibration: tau_us measured with a sound dead ahead. It is subtracted
// from every later estimate and cancels any fixed offset between the channels.
void doa_set_zero_offset_us(float tau_us);

#ifdef __cplusplus
}
#endif
