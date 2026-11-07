// Sound compass: points to a sound using the board's two microphones.
//
//   core 1  audio_task   read mics -> direction finding -> publish
//   core 0  LVGL + ui    draw the latest result about 30 times a second
//   core 0  log_task     send one line of results over USB, 10 times a second
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "doa.h"
#include "ui.h"

static const char *TAG = "compass";

#define SLOTS        BSP_AUDIO_TDM_SLOT_COUNT   // MIC1, reference, MIC2, unused
#define SLOT_LEFT    BSP_AUDIO_TDM_SLOT_FL
#define SLOT_RIGHT   BSP_AUDIO_TDM_SLOT_FR
#define MIC_GAIN_DB  24.0f
#define CAL_SECONDS  5.0f
#define NVS_SPACE    "compass"
#define NVS_ZERO_KEY "zero_us"

// Latest results, written by the audio task and read by the UI and the logger.
typedef struct {
    ui_state_t ui;
    uint32_t   frames;
    uint32_t   dsp_us;
    float      frame_angle;
    bool       frame_sound;
    float      zero_us;
} shared_t;

static shared_t s_shared;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_cal_requested;
static volatile int  s_notice;       // 0 none, 1 calibrating, 2 done, 3 failed
static esp_codec_dev_handle_t s_mic;

static float s_mic_l[DOA_FFT_N], s_mic_r[DOA_FFT_N];
static int16_t s_raw[DOA_HOP * SLOTS];

static float load_zero_offset(void)
{
    nvs_handle_t h;
    int32_t centi_us = 0;
    if (nvs_open(NVS_SPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, NVS_ZERO_KEY, &centi_us);
        nvs_close(h);
    }
    return (float)centi_us / 100.0f;
}

static void save_zero_offset(float tau_us)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_ZERO_KEY, (int32_t)lroundf(tau_us * 100.0f));
        nvs_commit(h);
        nvs_close(h);
    }
}

static esp_err_t audio_start(void)
{
    ESP_RETURN_ON_ERROR(bsp_audio_init_voice_24k(), TAG, "audio bus");

    // The board shares its audio clocks with the speaker codec, so it has to be
    // opened too. It stays muted and nothing is ever played.
    esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(speaker && s_mic, ESP_FAIL, TAG, "codec init");

    esp_codec_dev_sample_info_t out_fs = {
        .sample_rate = DOA_SAMPLE_RATE_HZ,
        .channel = 2,
        .bits_per_sample = 16,
    };
    esp_codec_dev_sample_info_t in_fs = {
        .sample_rate = DOA_SAMPLE_RATE_HZ,
        .channel = SLOTS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(speaker, &out_fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "speaker open");
    esp_codec_dev_set_out_mute(speaker, true);
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_mic, &in_fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "mic open");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_channel_gain(s_mic, BSP_AUDIO_ES7210_MIC_MASK_FL_FR, MIC_GAIN_DB) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "mic gain");
    return ESP_OK;
}

static void audio_task(void *arg)
{
    (void)arg;
    doa_config_t cfg = doa_default_config();
    doa_init(&cfg);

    float zero_us = load_zero_offset();
    doa_set_zero_offset_us(zero_us);
    doa_track_t track;
    doa_track_reset(&track);
    ESP_LOGI(TAG, "spacing %.1f mm, zero offset %.1f us", cfg.mic_spacing_m * 1000.0f, zero_us);

    float cal_sum = 0.0f;
    int cal_frames = 0, cal_left = 0;
    const int cal_total = (int)(CAL_SECONDS * DOA_SAMPLE_RATE_HZ / DOA_HOP);
    uint32_t frames = 0;

    while (true) {
        if (esp_codec_dev_read(s_mic, s_raw, sizeof(s_raw)) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic read failed");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        memmove(s_mic_l, s_mic_l + DOA_HOP, (DOA_FFT_N - DOA_HOP) * sizeof(float));
        memmove(s_mic_r, s_mic_r + DOA_HOP, (DOA_FFT_N - DOA_HOP) * sizeof(float));
        for (int i = 0; i < DOA_HOP; i++) {
            s_mic_l[DOA_FFT_N - DOA_HOP + i] = (float)s_raw[i * SLOTS + SLOT_LEFT] / 32768.0f;
            s_mic_r[DOA_FFT_N - DOA_HOP + i] = (float)s_raw[i * SLOTS + SLOT_RIGHT] / 32768.0f;
        }

        if (s_cal_requested) {
            s_cal_requested = false;
            cal_sum = 0.0f;
            cal_frames = 0;
            cal_left = cal_total;
            doa_set_zero_offset_us(0.0f);
            s_notice = 1;
        }

        int64_t t0 = esp_timer_get_time();
        doa_frame_t frame;
        doa_process(s_mic_l, s_mic_r, &frame);
        doa_track_update(&track, &frame);
        uint32_t dsp_us = (uint32_t)(esp_timer_get_time() - t0);

        if (cal_left > 0) {
            if (frame.sound) {
                cal_sum += frame.tau_us;
                cal_frames++;
            }
            if (--cal_left == 0) {
                if (cal_frames >= 4) {
                    zero_us = cal_sum / (float)cal_frames;
                    save_zero_offset(zero_us);
                    s_notice = 2;
                } else {
                    s_notice = 3;
                }
                doa_set_zero_offset_us(zero_us);
            }
        }

        frames++;
        taskENTER_CRITICAL(&s_lock);
        s_shared.ui.tracking = track.valid;
        s_shared.ui.live = frame.sound;
        s_shared.ui.angle_deg = track.angle_deg;
        s_shared.ui.confidence = track.confidence;
        s_shared.ui.age_s = track.age_s;
        if (frame.sound) {
            s_shared.ui.tau_us = frame.tau_us;
        }
        s_shared.ui.level_db[0] = frame.level_db_mic1;
        s_shared.ui.level_db[1] = frame.level_db_mic2;
        s_shared.frames = frames;
        s_shared.dsp_us = dsp_us;
        s_shared.frame_angle = frame.angle_deg;
        s_shared.frame_sound = frame.sound;
        s_shared.zero_us = zero_us;
        taskEXIT_CRITICAL(&s_lock);
    }
}

// CSV over USB for tools/monitor.py. Kept off the audio task, because a USB write
// can stall for tens of milliseconds when no computer is reading.
static void log_task(void *arg)
{
    (void)arg;
    printf("DOA,ms,sound,frame_deg,bearing_deg,valid,conf,tau_us,left_db,right_db,dsp_us,frames\n");
    while (true) {
        shared_t s;
        taskENTER_CRITICAL(&s_lock);
        s = s_shared;
        taskEXIT_CRITICAL(&s_lock);
        printf("DOA,%lu,%d,%.1f,%.1f,%d,%.2f,%.1f,%.1f,%.1f,%lu,%lu\n",
               (unsigned long)(esp_timer_get_time() / 1000), s.frame_sound, s.frame_angle,
               s.ui.angle_deg, s.ui.tracking, s.ui.confidence, s.ui.tau_us,
               s.ui.level_db[0], s.ui.level_db[1], (unsigned long)s.dsp_us, (unsigned long)s.frames);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void on_long_press(void)
{
    s_cal_requested = true;
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    static int64_t last_us;
    static int shown_notice;
    static int64_t notice_until_us;
    int64_t now = esp_timer_get_time();
    float dt = last_us ? (float)(now - last_us) * 1e-6f : 0.03f;
    last_us = now;

    int notice = s_notice;
    if (notice != shown_notice) {
        shown_notice = notice;
        if (notice == 1) {
            ui_set_notice("ZEROING", "clap straight ahead");
        } else if (notice == 2) {
            ui_set_notice("ZERO SET", "saved on the board");
            notice_until_us = now + 1500000;
        } else if (notice == 3) {
            ui_set_notice("NOT ENOUGH SOUND", "hold to try again");
            notice_until_us = now + 2500000;
        }
    }
    if (notice_until_us && now > notice_until_us) {
        notice_until_us = 0;
        s_notice = 0;
        shown_notice = 0;
        ui_set_notice(NULL, NULL);
    }

    ui_state_t s;
    taskENTER_CRITICAL(&s_lock);
    s = s_shared.ui;
    taskEXIT_CRITICAL(&s_lock);
    ui_update(&s, dt);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    lv_display_t *display = bsp_display_start();
    assert(display);

    bsp_display_lock(0);
    ui_init(lv_screen_active(), on_long_press);
    lv_timer_create(ui_timer_cb, 33, NULL);
    bsp_display_unlock();

    ESP_ERROR_CHECK(audio_start());

    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 8, NULL, 1);
    xTaskCreatePinnedToCore(log_task, "log", 4096, NULL, 1, NULL, 0);
}
