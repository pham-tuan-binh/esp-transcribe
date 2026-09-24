#include "watcher.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_spd2010.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "watcher";

// General I2C bus: IO expander and audio codecs
#define I2C_PORT I2C_NUM_0
#define I2C_SDA  GPIO_NUM_47
#define I2C_SCL  GPIO_NUM_48

// PCA9535 IO expander. Port 0 is all inputs, port 1 mostly power switches.
#define EXP_ADDR        0x21
#define EXP_REG_INPUT0  0x00
#define EXP_REG_OUTPUT1 0x03
#define EXP_REG_CONFIG0 0x06
#define EXP_REG_CONFIG1 0x07
#define EXP_KNOB_BTN    BIT(3) // P0.3, low while pressed
#define EXP_PWR_LCD     BIT(1) // P1.1
#define EXP_PWR_SYSTEM  BIT(2) // P1.2
#define EXP_PWR_CODEC   BIT(4) // P1.4, codec and speaker amplifier
#define EXP_BAT_DET     BIT(5) // P1.5, the only input on port 1

// Audio: ES7243E (or ES7243 on early units) microphone ADC on I2S0
#define I2S_MCLK      GPIO_NUM_10
#define I2S_BCLK      GPIO_NUM_11
#define I2S_WS        GPIO_NUM_12
#define I2S_DIN       GPIO_NUM_15
#define ES7243_ADDR   0x13
#define ES7243E_ADDR  0x14

// Display: SPD2010, 412x412 over QSPI
#define LCD_SPI_HOST  SPI3_HOST
#define LCD_PCLK      GPIO_NUM_7
#define LCD_DATA0     GPIO_NUM_9
#define LCD_DATA1     GPIO_NUM_1
#define LCD_DATA2     GPIO_NUM_14
#define LCD_DATA3     GPIO_NUM_13
#define LCD_CS        GPIO_NUM_45
#define LCD_BL        GPIO_NUM_8
#define LCD_DRAW_ROWS 24 // LVGL draw buffer height, in DMA-capable internal RAM

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_exp;

static esp_err_t exp_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_exp, buf, sizeof(buf), 100);
}

static esp_err_t exp_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_exp, &reg, 1, value, 1, 100);
}

esp_err_t watcher_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "I2C bus");

    const i2c_device_config_t exp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &exp_cfg, &s_exp), TAG, "IO expander");

    // Same sequence as the BSP: keep the system rail on, power cycle the rest so the
    // display (which has no reset pin) starts clean, then switch on what we use.
    // The SD card, the Himax AI chip, Grove and the battery ADC stay off.
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_OUTPUT1, EXP_PWR_SYSTEM), TAG, "IO expander write");
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_CONFIG0, 0xff), TAG, "IO expander config");
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_CONFIG1, EXP_BAT_DET), TAG, "IO expander config");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_OUTPUT1, EXP_PWR_SYSTEM | EXP_PWR_LCD | EXP_PWR_CODEC), TAG,
                        "IO expander write");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

bool watcher_knob_pressed(void)
{
    uint8_t in = 0xff;
    if (exp_read(EXP_REG_INPUT0, &in) != ESP_OK) {
        return false;
    }
    return (in & EXP_KNOB_BTN) == 0;
}

esp_codec_dev_handle_t watcher_mic_open(float gain_db)
{
    // Receive only; the I2S master still drives MCLK/BCLK/WS for the ADC
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = GPIO_NUM_NC,
            .din = I2S_DIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg)); // esp_codec_dev enables it on open

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    // Early units have an ES7243, later ones an ES7243E
    const bool es7243 = i2c_master_probe(s_i2c_bus, ES7243_ADDR, 50) == ESP_OK;
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_PORT,
        .addr = (es7243 ? ES7243_ADDR : ES7243E_ADDR) << 1,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_if_t *codec_if;
    if (es7243) {
        es7243_codec_cfg_t cfg = {.ctrl_if = ctrl_if};
        codec_if = es7243_codec_new(&cfg);
    } else {
        es7243e_codec_cfg_t cfg = {.ctrl_if = ctrl_if};
        codec_if = es7243e_codec_new(&cfg);
    }
    ESP_LOGI(TAG, "Microphone ADC: %s", es7243 ? "ES7243" : "ES7243E");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    esp_codec_dev_handle_t mic = esp_codec_dev_new(&dev_cfg);
    assert(mic);

    // The microphone is on the right I2S slot: read both, keep channel 1
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 16000,
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(mic, &fs));
    esp_codec_dev_set_in_gain(mic, gain_db);
    return mic;
}

// The SPD2010 only accepts windows whose x range starts and ends on a multiple of 4
static void lcd_rounder_cb(lv_event_t *e)
{
    lv_area_t *area = lv_event_get_param(e);
    area->x1 &= ~3;
    area->x2 |= 3;
}

lv_display_t *watcher_display_start(void)
{
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PCLK,
        .data0_io_num = LCD_DATA0,
        .data1_io_num = LCD_DATA1,
        .data2_io_num = LCD_DATA2,
        .data3_io_num = LCD_DATA3,
        .max_transfer_sz = WATCHER_LCD_H_RES * LCD_DRAW_ROWS * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 3,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 2,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    spd2010_vendor_config_t vendor_cfg = {.flags.use_qspi_interface = 1};
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_spd2010(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = WATCHER_LCD_H_RES * LCD_DRAW_ROWS,
        .double_buffer = true,
        .hres = WATCHER_LCD_H_RES,
        .vres = WATCHER_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    assert(disp);
    lv_display_add_event_cb(disp, lcd_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    // Backlight on last, once there is something on the screen
    gpio_reset_pin(LCD_BL);
    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 1);
    return disp;
}
