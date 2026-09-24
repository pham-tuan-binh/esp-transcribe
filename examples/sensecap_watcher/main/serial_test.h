// Serial test commands on the console UART, driven by serial_test.py:
//   PCM <n_samples>\n + raw int16 audio  -> transcribes it, replies "TEXT: ..."
//   DUMP\n                               -> sends back the last knob recording as hex
//   REC <ms>\n                           -> acts as if the knob were held for <ms>
//   CH <0|1>\n                           -> records from the left (0) or right (1) I2S slot
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Serializes esp_transcribe_run() between the knob and the serial commands. */
esp_err_t transcribe_locked(const int16_t *pcm, size_t n_samples, char *text, size_t text_size);

/** Keeps a copy of the last knob recording for DUMP. */
void serial_test_save_recording(const int16_t *pcm, size_t n_samples);

/** True while a REC command holds the virtual knob down. */
bool serial_test_holding(void);

void serial_test_start(esp_codec_dev_handle_t mic);

#ifdef __cplusplus
}
#endif
