// Feeds audio from your own source into esp-transcribe, using the C++ wrapper.
// Here the "source" is a generated tone; replace ReadAudio() with your mic driver,
// a network stream, a file, etc. Audio must be 16kHz mono int16, fed in real time.
#include <cmath>
#include <cstdio>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_transcribe.hpp"

static constexpr size_t kFrameSamples = 320; // 20ms

static void ReadAudio(int16_t *frame, size_t n)
{
    static uint32_t t = 0;
    for (size_t i = 0; i < n; i++, t++) {
        frame[i] = static_cast<int16_t>(2000 * std::sin(2 * M_PI * 440 * t / ESP_TRANSCRIBE_SAMPLE_RATE));
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // real-time pacing
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(esp_transcribe::StartStream(
        [](std::string_view text) { printf("Heard: %.*s\n", static_cast<int>(text.size()), text.data()); },
        esp_transcribe::DefaultConfig(),
        [](esp_transcribe_state_t state) { printf("State: %d\n", state); }));

    int16_t frame[kFrameSamples];
    while (true) {
        ReadAudio(frame, kFrameSamples);
        esp_transcribe::Feed(frame, kFrameSamples);
    }
}
