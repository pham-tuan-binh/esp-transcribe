// Minimal board layer for the SenseCAP Watcher: just what this example needs
// (power rails, knob button, microphone, display). Pins and init sequences come
// from Seeed's BSP (SenseCAP-Watcher-Firmware/components/sensecap-watcher),
// rewritten for the ESP-IDF 6 drivers and LVGL 9.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_codec_dev.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WATCHER_LCD_H_RES 412
#define WATCHER_LCD_V_RES 412

/** I2C bus and IO expander, powers the display and the audio codec. Call first. */
esp_err_t watcher_init(void);

/** True while the knob is pressed down. Reads the IO expander over I2C. */
bool watcher_knob_pressed(void);

/** Opens the microphone at 16kHz, 16-bit, mono. */
esp_codec_dev_handle_t watcher_mic_open(float gain_db);

/** Starts the display and LVGL (esp_lvgl_port). Use lvgl_port_lock() around LVGL calls. */
lv_display_t *watcher_display_start(void);

#ifdef __cplusplus
}
#endif
