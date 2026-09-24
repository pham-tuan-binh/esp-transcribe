#include "serial_test.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_transcribe.h"

static const char *TAG = "serial_test";

#define UART_PORT UART_NUM_0 // the console, on the Watcher's USB-serial port

static SemaphoreHandle_t s_lock;
static int16_t *s_rec;    // last knob recording
static size_t s_rec_n;
static int16_t *s_pcm;    // audio received over serial
static volatile int64_t s_hold_until; // REC: virtual knob held until this time (us)

bool serial_test_holding(void)
{
    return esp_timer_get_time() < s_hold_until;
}

esp_err_t transcribe_locked(const int16_t *pcm, size_t n_samples, char *text, size_t text_size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = esp_transcribe_run(pcm, n_samples, text, text_size);
    xSemaphoreGive(s_lock);
    return err;
}

void serial_test_save_recording(const int16_t *pcm, size_t n_samples)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_rec, pcm, n_samples * sizeof(int16_t));
    s_rec_n = n_samples;
    xSemaphoreGive(s_lock);
}

static int read_line(char *buf, size_t size)
{
    size_t n = 0;
    while (n + 1 < size) {
        char c;
        if (uart_read_bytes(UART_PORT, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n') {
            break;
        }
        if (c != '\r') {
            buf[n++] = c;
        }
    }
    buf[n] = '\0';
    return n;
}

static void cmd_pcm(size_t n_samples, bool ptt)
{
    static char text[512];
    if (n_samples == 0 || n_samples > ESP_TRANSCRIBE_MAX_SAMPLES) {
        printf("ERR bad length, max %d samples\n", ESP_TRANSCRIBE_MAX_SAMPLES);
        return;
    }
    printf("READY\n");
    fflush(stdout);

    const size_t bytes = n_samples * sizeof(int16_t);
    size_t got = 0;
    while (got < bytes) {
        const int n = uart_read_bytes(UART_PORT, (uint8_t *)s_pcm + got, bytes - got, pdMS_TO_TICKS(3000));
        if (n <= 0) {
            printf("ERR timeout after %u of %u bytes\n", (unsigned)got, (unsigned)bytes);
            return;
        }
        got += n;
    }
    uint32_t sum = 0;
    int peak = 0;
    for (size_t i = 0; i < bytes; i++) {
        sum += ((uint8_t *)s_pcm)[i];
    }
    for (size_t i = 0; i < n_samples; i++) {
        peak = MAX(peak, abs(s_pcm[i]));
    }
    printf("GOT %u bytes, sum %lu, peak %d\n", (unsigned)bytes, (unsigned long)sum, peak);
    fflush(stdout);

    esp_err_t err;
    int64_t start;
    if (ptt) {
        // Push-to-talk path: push 20ms frames in real time, as if someone were talking,
        // then time only what is left after the "release"
        err = esp_transcribe_begin();
        for (size_t i = 0; err == ESP_OK && i < n_samples; i += 320) {
            err = esp_transcribe_push(s_pcm + i, MIN(320, n_samples - i));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        start = esp_timer_get_time();
        if (err == ESP_OK) {
            err = esp_transcribe_finish(text, sizeof(text));
        } else {
            esp_transcribe_cancel();
        }
    } else {
        start = esp_timer_get_time();
        err = transcribe_locked(s_pcm, n_samples, text, sizeof(text));
    }
    const float took_s = (esp_timer_get_time() - start) / 1e6f;
    if (err != ESP_OK) {
        printf("ERR %s\n", esp_err_to_name(err));
    } else {
        printf("TIME %.2f\nTEXT: %s\n", took_s, text);
    }
}

static void cmd_dump(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    printf("DUMP %u\n", (unsigned)s_rec_n);
    char line[2 * 64 + 2];
    for (size_t i = 0; i < s_rec_n; i += 32) {
        const size_t n = MIN(32, s_rec_n - i);
        for (size_t j = 0; j < n; j++) {
            const uint16_t s = (uint16_t)s_rec[i + j];
            snprintf(line + 4 * j, 5, "%02x%02x", s & 0xff, s >> 8);
        }
        printf("%s\n", line);
    }
    printf("END\n");
    fflush(stdout);
    xSemaphoreGive(s_lock);
}

static void serial_task(void *arg)
{
    char line[64];
    for (;;) {
        if (read_line(line, sizeof(line)) == 0) {
            continue;
        }
        if (strncmp(line, "PCM ", 4) == 0) {
            cmd_pcm(strtoul(line + 4, NULL, 10), false);
        } else if (strncmp(line, "PTT ", 4) == 0) {
            cmd_pcm(strtoul(line + 4, NULL, 10), true);
        } else if (strncmp(line, "REC ", 4) == 0) {
            s_hold_until = esp_timer_get_time() + 1000LL * strtoul(line + 4, NULL, 10);
            printf("OK\n");
        } else if (strcmp(line, "DUMP") == 0) {
            cmd_dump();
        } else {
            printf("ERR unknown command '%s'\n", line);
        }
        fflush(stdout);
    }
}

void serial_test_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_rec = heap_caps_malloc(ESP_TRANSCRIBE_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_pcm = heap_caps_malloc(ESP_TRANSCRIBE_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    assert(s_lock && s_rec && s_pcm);
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 4 * 1024, 0, 0, NULL, 0));
    xTaskCreate(serial_task, "serial_test", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Serial test commands ready (see serial_test.py)");
}
