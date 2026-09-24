#include <stdio.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_transcribe.h"

// User LED of the XIAO ESP32-S3 (active low). Set to GPIO_NUM_NC if your board has none.
#define LED_GPIO GPIO_NUM_21

static void on_text(const char *text, void *user_ctx)
{
    printf("Heard: %s\n", text);
}

static void on_state(esp_transcribe_state_t state, void *user_ctx)
{
    if (LED_GPIO != GPIO_NUM_NC) {
        gpio_set_level(LED_GPIO, state == ESP_TRANSCRIBE_STATE_IDLE);
    }
}

void app_main(void)
{
    if (LED_GPIO != GPIO_NUM_NC) {
        gpio_reset_pin(LED_GPIO);
        gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(LED_GPIO, 1);
    }

    // Defaults come from menuconfig (Component config -> esp-transcribe); override anything here.
    esp_transcribe_config_t cfg = ESP_TRANSCRIBE_DEFAULT_CONFIG();
    cfg.mic.bclk_gpio = 2; // INMP441 SCK
    cfg.mic.ws_gpio = 3;   // INMP441 WS
    cfg.mic.din_gpio = 1;  // INMP441 SD
    cfg.on_text = on_text;
    cfg.on_state = on_state;
    ESP_ERROR_CHECK(esp_transcribe_start_mic(&cfg));

    printf("Listening, say something...\n");
}
