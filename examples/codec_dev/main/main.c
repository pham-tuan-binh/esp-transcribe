// Microphone from a codec board, handed over as an esp_codec_dev record device,
// exactly like the LiveKit ESP32 SDK examples get theirs.
#include <stdio.h>

#include "esp_err.h"
#include "codec_board.h"
#include "codec_init.h"
#include "esp_transcribe_codec_dev.h"

static void on_text(const char *text, void *user_ctx)
{
    printf("Heard: %s\n", text);
}

void app_main(void)
{
    // Board layer: same calls as board_init() in the LiveKit examples
    set_codec_board_type(CONFIG_EXAMPLE_CODEC_BOARD_TYPE);
    codec_init_cfg_t codec_cfg = {
#if CONFIG_EXAMPLE_MIC_USE_TDM
        .in_mode = CODEC_I2S_MODE_TDM,
        .in_use_tdm = true,
#endif
        .reuse_dev = false,
    };
    ESP_ERROR_CHECK(init_codec(&codec_cfg));
    esp_codec_dev_handle_t record_handle = get_record_handle();
    esp_codec_dev_set_in_gain(record_handle, 30.0);

    // Hand the record device to esp-transcribe
    esp_transcribe_config_t cfg = ESP_TRANSCRIBE_DEFAULT_CONFIG();
    cfg.on_text = on_text;
    esp_transcribe_codec_dev_config_t dev = ESP_TRANSCRIBE_CODEC_DEV_DEFAULT_CONFIG(record_handle);
    dev.channels = CONFIG_EXAMPLE_MIC_CHANNELS;
    dev.channel = 0;
    ESP_ERROR_CHECK(esp_transcribe_start_codec_dev(&dev, &cfg));

    printf("Listening, say something...\n");
}
