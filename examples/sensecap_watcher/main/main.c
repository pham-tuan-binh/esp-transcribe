// Push-to-talk on the SenseCAP Watcher: hold the knob down, speak, let go, and the
// transcription shows up on the round display. Up to 8.3s per message.
//
// Audio is recorded into a PSRAM buffer while the knob is held, then transcribed
// with esp_transcribe_run(), so no voice activity detection is involved.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_transcribe.h"
#include "serial_test.h"
#include "watcher.h"

static const char *TAG = "ptt";

#define FRAME_SAMPLES   320                                // 20ms per microphone read
#define TRIM_SAMPLES    (ESP_TRANSCRIBE_SAMPLE_RATE / 10)  // 100ms cut from each end: the knob clicks
#define MIN_SAMPLES     (ESP_TRANSCRIBE_SAMPLE_RATE / 2)   // ignore taps shorter than 500ms
#define NORM_WINDOW     (ESP_TRANSCRIBE_SAMPLE_RATE / 10)  // 100ms
#define NORM_RMS        3000.0f
#define NORM_MAX_GAIN   8.0f
#define RELEASE_FRAMES  2                                  // knob up this many reads in a row = released
#define MAX_MS          (ESP_TRANSCRIBE_MAX_SAMPLES * 1000 / ESP_TRANSCRIBE_SAMPLE_RATE)

#define COLOR_IDLE      lv_color_hex(0x8a8f98)
#define COLOR_LISTEN    lv_color_hex(0xff4d4d)
#define COLOR_BUSY      lv_color_hex(0xffb020)

static lv_obj_t *s_arc;
static lv_obj_t *s_status;
static lv_obj_t *s_text;
static lv_obj_t *s_info;

static void ui_create(void)
{
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Recording time, around the edge of the round screen
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, WATCHER_LCD_H_RES - 8, WATCHER_LCD_V_RES - 8);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, MAX_MS);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x1c1f24), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, COLOR_LISTEN, LV_PART_INDICATOR);

    s_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_18, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 70);

    s_text = lv_label_create(scr);
    lv_obj_set_width(s_text, 300);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_text, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_text, lv_color_white(), 0);
    lv_obj_center(s_text);
    lv_label_set_text(s_text, "");

    s_info = lv_label_create(scr);
    lv_obj_set_style_text_color(s_info, COLOR_IDLE, 0);
    lv_obj_align(s_info, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_label_set_text(s_info, "");
    lvgl_port_unlock();
}

static void ui_status(const char *status, lv_color_t color)
{
    lvgl_port_lock(0);
    lv_label_set_text(s_status, status);
    lv_obj_set_style_text_color(s_status, color, 0);
    lvgl_port_unlock();
}

static void ui_result(const char *text, lv_color_t color, const char *info)
{
    lvgl_port_lock(0);
    lv_label_set_text(s_text, text);
    lv_obj_set_style_text_color(s_text, color, 0);
    lv_label_set_text(s_info, info);
    lvgl_port_unlock();
}

static void ui_progress(uint32_t ms, lv_color_t color)
{
    lvgl_port_lock(0);
    lv_arc_set_value(s_arc, ms);
    lv_obj_set_style_arc_color(s_arc, color, LV_PART_INDICATOR);
    lvgl_port_unlock();
}

// Transcription keeps both cores busy, so draw the screen now, before it starts
static void ui_refresh_now(void)
{
    lvgl_port_lock(0);
    lv_refr_now(NULL);
    lvgl_port_unlock();
}

static void ui_idle(void)
{
    ui_status("Hold the knob and speak", COLOR_IDLE);
    ui_progress(0, COLOR_LISTEN);
}

static void transcribe(int16_t *pcm, size_t n_samples)
{
    static char text[512];
    char info[48];

    if (n_samples > MIN_SAMPLES + 2 * TRIM_SAMPLES) {
        pcm += TRIM_SAMPLES;
        n_samples -= 2 * TRIM_SAMPLES;
    }
    const float audio_s = (float)n_samples / ESP_TRANSCRIBE_SAMPLE_RATE;

    // Remove the DC offset and bring speech to a consistent level: the loudest 100ms
    // window is scaled to NORM_RMS, boosting by at most NORM_MAX_GAIN
    int64_t sum = 0;
    for (size_t i = 0; i < n_samples; i++) {
        sum += pcm[i];
    }
    const int dc = sum / (int64_t)n_samples;
    float loudest = 1;
    for (size_t i = 0; i + NORM_WINDOW <= n_samples; i += NORM_WINDOW / 2) {
        int64_t energy = 0;
        for (size_t j = i; j < i + NORM_WINDOW; j++) {
            energy += (int64_t)(pcm[j] - dc) * (pcm[j] - dc);
        }
        loudest = MAX(loudest, sqrtf((float)energy / NORM_WINDOW));
    }
    const float gain = MIN(MAX(NORM_RMS / loudest, 1.0f), NORM_MAX_GAIN);
    for (size_t i = 0; i < n_samples; i++) {
        pcm[i] = (int16_t)MIN(MAX((pcm[i] - dc) * gain, INT16_MIN), INT16_MAX);
    }
    // To tune CONFIG_EXAMPLE_MIC_GAIN_DB: speech should reach a loudest RMS of a few thousand
    ESP_LOGI(TAG, "Recorded %.1fs, loudest RMS %.0f, boosted x%.1f", audio_s, loudest, gain);
    serial_test_save_recording(pcm, n_samples);

    ui_status("Transcribing...", COLOR_BUSY);
    ui_progress(n_samples * 1000 / ESP_TRANSCRIBE_SAMPLE_RATE, COLOR_BUSY);
    ui_refresh_now();

    const int64_t start = esp_timer_get_time();
    const esp_err_t err = transcribe_locked(pcm, n_samples, text, sizeof(text));
    const float took_s = (esp_timer_get_time() - start) / 1e6f;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Transcription failed: %s", esp_err_to_name(err));
        ui_result(esp_err_to_name(err), COLOR_LISTEN, "");
    } else if (text[0] == '\0') {
        ESP_LOGI(TAG, "Heard nothing (%.1fs audio, %.1fs)", audio_s, took_s);
        snprintf(info, sizeof(info), "%.1fs audio in %.1fs", audio_s, took_s);
        ui_result("(didn't catch that)", COLOR_IDLE, info);
    } else {
        ESP_LOGI(TAG, "Heard (%.1fs audio, %.1fs): %s", audio_s, took_s, text);
        text[0] = toupper((unsigned char)text[0]);
        snprintf(info, sizeof(info), "%.1fs audio in %.1fs", audio_s, took_s);
        ui_result(text, lv_color_white(), info);
    }
    ui_idle();
}

static void push_to_talk_task(void *arg)
{
    esp_codec_dev_handle_t mic = arg;
    int16_t *audio = heap_caps_malloc(ESP_TRANSCRIBE_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    assert(audio);
    int16_t frame[FRAME_SAMPLES];

    bool recording = false;
    bool wait_release = false; // hit the time limit, don't start again until the knob comes up
    int up_frames = 0;
    size_t n = 0;

    ui_idle();
    for (;;) {
        // Read the microphone all the time, so the recording starts with fresh audio
        if (esp_codec_dev_read(mic, frame, sizeof(frame)) != ESP_CODEC_DEV_OK) {
            continue;
        }
        const bool down = watcher_knob_pressed() || serial_test_holding();
        up_frames = down ? 0 : up_frames + 1;

        if (!recording) {
            if (down && !wait_release) {
                recording = true;
                n = 0;
                ui_status("Listening...", COLOR_LISTEN);
                ui_result("", lv_color_white(), "");
            } else if (!down) {
                wait_release = false;
            }
        }
        if (!recording) {
            continue;
        }

        memcpy(audio + n, frame, sizeof(frame));
        n += FRAME_SAMPLES;
        const bool full = n + FRAME_SAMPLES > ESP_TRANSCRIBE_MAX_SAMPLES;

        if (up_frames >= RELEASE_FRAMES || full) {
            recording = false;
            wait_release = full;
            if (n < MIN_SAMPLES) {
                ui_idle();
            } else {
                transcribe(audio, n);
            }
        } else if ((n / FRAME_SAMPLES) % 5 == 0) {
            ui_progress(n * 1000 / ESP_TRANSCRIBE_SAMPLE_RATE, COLOR_LISTEN);
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(watcher_init());
    watcher_display_start();
    ui_create();

    ui_status("Loading model...", COLOR_BUSY);
    const esp_err_t err = esp_transcribe_init();
    if (err != ESP_OK) {
        // ESP_ERR_INVALID_STATE usually means the model partition was never flashed
        ESP_LOGE(TAG, "esp_transcribe_init: %s", esp_err_to_name(err));
        ui_status("Model not loaded", COLOR_LISTEN);
        ui_result(esp_err_to_name(err), COLOR_LISTEN, "Flash the model: idf.py flash");
        return;
    }

    esp_codec_dev_handle_t mic = watcher_mic_open(CONFIG_EXAMPLE_MIC_GAIN_DB);
    serial_test_start(mic);
    xTaskCreate(push_to_talk_task, "ptt", 8192, mic, 5, NULL);
}
