#include "microphone.h"

#include <algorithm>
#include <limits>

#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>

#include <config.h>

static const char *kTag{"esp_transcribe"};

Microphone::Microphone(const int port, const gpio_num_t bclk_pin, const gpio_num_t ws_pin,
                       const gpio_num_t din_pin, const uint8_t gain)
    : gain_{gain}
{
    const i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);

    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &i2s_rx_channel_));

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSamplingRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk_pin,
            .ws = ws_pin,
            .dout = I2S_GPIO_UNUSED,
            .din = din_pin,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    rx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_channel_, &rx_std_cfg));

    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_channel_));
}

size_t Microphone::Read(tlib::Tensor<int16_t> &buffer)
{
    size_t n_bytes_read{0};

    int16_t *buffer_data_i16{buffer.Data()};
    int32_t *buffer_data_i32{reinterpret_cast<int32_t *>(buffer_data_i16)};
    uint8_t *buffer_data_u8{reinterpret_cast<uint8_t *>(buffer_data_i16)};

    // The I2S RX channel config reads from the microphone with 32 bit sample width which is directly written into the buffer
    const esp_err_t err = i2s_channel_read(i2s_rx_channel_, buffer_data_u8, buffer.Bytes(), &n_bytes_read, portMAX_DELAY);

    if (err != ESP_OK)
    {
        ESP_LOGE(kTag, "Error reading from I2S microphone: %s", esp_err_to_name(err));
        return 0;
    }

    if (n_bytes_read == 0)
    {
        ESP_LOGW(kTag, "Read 0 bytes from I2S microphone");
        return 0;
    }

    // Convert 32 bit samples to 16 bit samples
    const size_t samples_read{n_bytes_read / sizeof(int32_t)};
    for (size_t i = 0; i < samples_read; i++)
    {
        int32_t temp = buffer_data_i32[i] >> gain_;
        buffer_data_i16[i] = std::clamp<int32_t>(temp, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max());
    }

    return samples_read;
}