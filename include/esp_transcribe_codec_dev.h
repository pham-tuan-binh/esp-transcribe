/**
 * Use an esp_codec_dev record device as the audio source for esp-transcribe, the
 * same way the LiveKit ESP32 SDK takes its microphone. Get the handle from
 * tempotian/codec_board (get_record_handle()), from a BSP, or build it yourself
 * with esp_codec_dev_new().
 *
 *     esp_transcribe_config_t cfg = ESP_TRANSCRIBE_DEFAULT_CONFIG();
 *     cfg.on_text = on_text;
 *     esp_transcribe_codec_dev_config_t dev = ESP_TRANSCRIBE_CODEC_DEV_DEFAULT_CONFIG(get_record_handle());
 *     ESP_ERROR_CHECK(esp_transcribe_start_codec_dev(&dev, &cfg));
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_codec_dev.h"
#include "esp_transcribe.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Record device settings. */
typedef struct {
    esp_codec_dev_handle_t handle; /*!< Record device (ESP_CODEC_DEV_TYPE_IN) */
    uint8_t channels;              /*!< Interleaved channels to read, e.g. 2 or 4 for an ES7210 in TDM mode */
    uint16_t channel_mask;         /*!< Channel mask passed to esp_codec_dev_open(), 0 for all channels */
    uint8_t channel;               /*!< Index of the channel to transcribe, < channels */
    bool skip_open;                /*!< true if you already opened the device at 16kHz, 16-bit, `channels` channels */
} esp_transcribe_codec_dev_config_t;

/** Mono, opened by esp-transcribe. */
#define ESP_TRANSCRIBE_CODEC_DEV_DEFAULT_CONFIG(record_handle) { \
    .handle = (record_handle),                                 \
    .channels = 1,                                             \
    .channel_mask = 0,                                         \
    .channel = 0,                                              \
    .skip_open = false,                                        \
}

/**
 * Start the always-listening pipeline on an esp_codec_dev record device.
 * cfg->mic is ignored; set the input gain with esp_codec_dev_set_in_gain().
 *
 * esp-transcribe reads the device from its own task, so nothing else may read
 * it at the same time. To share one microphone with LiveKit, capture it once
 * and pass the audio to esp_transcribe_feed() instead.
 * Only one pipeline (mic, stream or codec_dev) can be started, once.
 */
esp_err_t esp_transcribe_start_codec_dev(const esp_transcribe_codec_dev_config_t *dev,
                                         const esp_transcribe_config_t *cfg);

#ifdef __cplusplus
}
#endif
