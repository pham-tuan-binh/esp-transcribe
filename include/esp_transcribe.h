/**
 * esp-transcribe: on-device English speech-to-text for the ESP32-S3.
 *
 * Runs a distilled and int8-quantized 13M parameter conformer-CTC model
 * (based on nvidia/stt_en_conformer_ctc_small) fully on the chip.
 *
 * Requirements: ESP32-S3 with >= 4MB PSRAM and >= 16MB flash, and a data
 * partition (default label "model", >= 14MB) holding the model.
 *
 * Ways to use it:
 *
 *  1. Always-listening I2S microphone (INMP441 or similar):
 *
 *         esp_transcribe_config_t cfg = ESP_TRANSCRIBE_DEFAULT_CONFIG();
 *         cfg.on_text = my_text_callback;
 *         ESP_ERROR_CHECK(esp_transcribe_start_mic(&cfg));
 *
 *  2. Always-listening on an esp_codec_dev record device, like the LiveKit ESP32 SDK
 *     (codec boards, BSPs, ES7210, ...). See esp_transcribe_codec_dev.h:
 *
 *         esp_transcribe_codec_dev_config_t dev = ESP_TRANSCRIBE_CODEC_DEV_DEFAULT_CONFIG(get_record_handle());
 *         ESP_ERROR_CHECK(esp_transcribe_start_codec_dev(&dev, &cfg));
 *
 *  3. Always-listening on audio you provide (another mic driver, a network stream, ...):
 *
 *         ESP_ERROR_CHECK(esp_transcribe_start_stream(&cfg));
 *         // then, from one task, as audio arrives:
 *         esp_transcribe_feed(pcm, n_samples, portMAX_DELAY);
 *
 *  4. One-shot transcription of a recorded buffer:
 *
 *         char text[256];
 *         ESP_ERROR_CHECK(esp_transcribe_run(pcm, n_samples, text, sizeof(text)));
 *
 *  5. Push-to-talk: the same, but preprocessed while you record, so the text is ready sooner:
 *
 *         esp_transcribe_begin();
 *         esp_transcribe_push(pcm, n_samples);   // as audio arrives
 *         esp_transcribe_finish(text, sizeof(text));
 *
 * All audio is 16kHz mono int16 PCM.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sample rate expected by the model (mono, int16). */
#define ESP_TRANSCRIBE_SAMPLE_RATE 16000

/** Audio is processed in chunks of this many samples (360ms). */
#define ESP_TRANSCRIBE_CHUNK_SAMPLES 5760

/** Maximum number of chunks per transcription (limited by the 256KB SRAM pool). */
#define ESP_TRANSCRIBE_MAX_CHUNKS 23

/** Maximum number of samples per transcription (23 chunks, ~8.3s). */
#define ESP_TRANSCRIBE_MAX_SAMPLES (ESP_TRANSCRIBE_CHUNK_SAMPLES * ESP_TRANSCRIBE_MAX_CHUNKS)

/** Pipeline state, reported through esp_transcribe_config_t::on_state. */
typedef enum {
    ESP_TRANSCRIBE_STATE_IDLE = 0,     /*!< Waiting for sound */
    ESP_TRANSCRIBE_STATE_LISTENING,    /*!< Sound detected, recording speech */
    ESP_TRANSCRIBE_STATE_TRANSCRIBING, /*!< Running the model */
} esp_transcribe_state_t;

/**
 * Called with the (lower-case) transcription of each utterance.
 * Runs on the inference task: keep it short, copy the text if you need it later.
 */
typedef void (*esp_transcribe_text_cb_t)(const char *text, void *user_ctx);

/** Called whenever the pipeline state changes. Runs on the inference task. */
typedef void (*esp_transcribe_state_cb_t)(esp_transcribe_state_t state, void *user_ctx);

/** Configuration of the always-listening pipeline. */
typedef struct {
    /** I2S microphone (INMP441 or similar, L/R pin tied to GND). Only used by esp_transcribe_start_mic(). */
    struct {
        int port;           /*!< I2S peripheral number */
        int bclk_gpio;      /*!< Bit clock pin (INMP441: SCK) */
        int ws_gpio;        /*!< Word select pin (INMP441: WS) */
        int din_gpio;       /*!< Data in pin (INMP441: SD) */
        uint8_t gain_shift; /*!< Right shift when converting 32-bit mic samples to 16-bit.
                                 Lower is louder: each step down doubles the gain. */
    } mic;

    /** Voice activity detection. Audio is split into 360ms chunks. */
    struct {
        uint32_t threshold;   /*!< Mean absolute int16 amplitude above which a chunk counts as sound */
        uint8_t min_chunks;   /*!< Minimum number of chunks with sound to trigger a transcription */
        uint8_t max_chunks;   /*!< Transcribe once this many chunks are recorded (<= ESP_TRANSCRIBE_MAX_CHUNKS) */
        uint8_t eos_chunks;   /*!< Consecutive silent chunks that end an utterance (> 1) */
        uint8_t prune_chunks; /*!< Trailing silent chunks dropped before transcription (< eos_chunks) */
    } vad;

    esp_transcribe_text_cb_t on_text;   /*!< Required: receives transcriptions */
    esp_transcribe_state_cb_t on_state; /*!< Optional: receives state changes, e.g. to drive an LED */
    void *user_ctx;                     /*!< Passed to the callbacks */
} esp_transcribe_config_t;

/** Defaults come from menuconfig (Component config -> esp-transcribe). */
#define ESP_TRANSCRIBE_DEFAULT_CONFIG() {                        \
    .mic = {                                                     \
        .port = CONFIG_ESP_TRANSCRIBE_I2S_PORT,                  \
        .bclk_gpio = CONFIG_ESP_TRANSCRIBE_I2S_BCLK_GPIO,        \
        .ws_gpio = CONFIG_ESP_TRANSCRIBE_I2S_WS_GPIO,            \
        .din_gpio = CONFIG_ESP_TRANSCRIBE_I2S_DIN_GPIO,          \
        .gain_shift = CONFIG_ESP_TRANSCRIBE_GAIN_SHIFT,          \
    },                                                           \
    .vad = {                                                     \
        .threshold = CONFIG_ESP_TRANSCRIBE_VAD_THRESHOLD,        \
        .min_chunks = CONFIG_ESP_TRANSCRIBE_MIN_CHUNKS,          \
        .max_chunks = CONFIG_ESP_TRANSCRIBE_MAX_CHUNKS,          \
        .eos_chunks = CONFIG_ESP_TRANSCRIBE_EOS_CHUNKS,          \
        .prune_chunks = CONFIG_ESP_TRANSCRIBE_PRUNE_CHUNKS,      \
    },                                                           \
    .on_text = NULL,                                             \
    .on_state = NULL,                                            \
    .user_ctx = NULL,                                            \
}

/**
 * Load the model and start the inference helper on the second core.
 * Safe to call more than once. Called implicitly by the other functions,
 * so you only need it to load the model early.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND if the model partition is missing,
 *         ESP_ERR_INVALID_STATE if the partition does not contain a valid model,
 *         ESP_ERR_NO_MEM if the 4MB PSRAM pool cannot be allocated.
 */
esp_err_t esp_transcribe_init(void);

/**
 * Start the always-listening pipeline on an I2S microphone. Each detected
 * utterance is transcribed and delivered to cfg->on_text.
 * Only one pipeline (mic, stream or codec_dev) can be started, once.
 */
esp_err_t esp_transcribe_start_mic(const esp_transcribe_config_t *cfg);

/**
 * Start the always-listening pipeline on audio pushed with esp_transcribe_feed().
 * cfg->mic is ignored. Only one pipeline (mic, stream or codec_dev) can be started, once.
 */
esp_err_t esp_transcribe_start_stream(const esp_transcribe_config_t *cfg);

/**
 * Push 16kHz mono int16 audio into the stream pipeline. Feed audio in real time,
 * from a single task. Audio that arrives while an utterance is being transcribed
 * is dropped, just like with the microphone pipeline.
 *
 * @param pcm         Audio samples
 * @param n_samples   Number of samples
 * @param timeout     Ticks to wait for buffer space
 *
 * @return Number of samples accepted (less than n_samples on timeout),
 *         0 if the stream pipeline is not running.
 */
size_t esp_transcribe_feed(const int16_t *pcm, size_t n_samples, uint32_t timeout);

/**
 * Transcribe a buffer of 16kHz mono int16 PCM audio. Blocks until done.
 * A trailing partial chunk is zero-padded.
 *
 * Not available while a listening pipeline is running.
 *
 * @param pcm        Audio samples
 * @param n_samples  Number of samples, 1..ESP_TRANSCRIBE_MAX_SAMPLES
 * @param text       Output buffer for the NUL-terminated transcription
 * @param text_size  Size of the output buffer; longer text is truncated
 */
esp_err_t esp_transcribe_run(const int16_t *pcm, size_t n_samples, char *text, size_t text_size);

/**
 * Incremental version of esp_transcribe_run(), for push-to-talk: push audio while it
 * is being recorded, and each complete 360ms chunk is preprocessed in the background.
 * esp_transcribe_finish() then only has the last chunk and the model left to run, which
 * saves about 0.5s per second of audio compared to esp_transcribe_run(). Same result.
 *
 *     esp_transcribe_begin();
 *     while (button_held) {
 *         read_mic(frame, 320);
 *         esp_transcribe_push(frame, 320);
 *     }
 *     esp_transcribe_finish(text, sizeof(text));
 *
 * Call begin, push and finish (or cancel) from the same task. The preprocessing task runs
 * at CONFIG_ESP_TRANSCRIBE_SESSION_PRIORITY, keep the recording task above it.
 * Not available while a listening pipeline is running.
 */
esp_err_t esp_transcribe_begin(void);

/**
 * Adds 16kHz mono int16 audio to the session. Only blocks if preprocessing falls behind.
 *
 * @return ESP_OK, ESP_ERR_INVALID_SIZE if the session would exceed ESP_TRANSCRIBE_MAX_SAMPLES
 *         (nothing is added), ESP_ERR_INVALID_STATE if no session is running.
 */
esp_err_t esp_transcribe_push(const int16_t *pcm, size_t n_samples);

/**
 * Ends the session and transcribes the audio pushed so far. Blocks until done.
 *
 * @return ESP_OK, ESP_ERR_INVALID_SIZE if no audio was pushed (the session is ended),
 *         ESP_ERR_INVALID_STATE if no session is running.
 */
esp_err_t esp_transcribe_finish(char *text, size_t text_size);

/** Ends the session without transcribing. Does nothing if no session is running. */
void esp_transcribe_cancel(void);

#ifdef __cplusplus
}
#endif
