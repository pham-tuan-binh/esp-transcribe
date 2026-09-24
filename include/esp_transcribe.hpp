/**
 * C++ convenience wrapper around esp_transcribe.h: lambdas instead of function
 * pointers, std::string instead of char buffers.
 *
 *     esp_transcribe::StartMic([](std::string_view text) {
 *         printf("Heard: %.*s\n", (int)text.size(), text.data());
 *     });
 */
#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "esp_transcribe.h"
#include "esp_transcribe_codec_dev.h"

namespace esp_transcribe
{

    using TextCallback = std::function<void(std::string_view text)>;
    using StateCallback = std::function<void(esp_transcribe_state_t state)>;

    /** Defaults from menuconfig. Callbacks are set by StartMic/StartStream. */
    inline esp_transcribe_config_t DefaultConfig(void)
    {
        esp_transcribe_config_t cfg = ESP_TRANSCRIBE_DEFAULT_CONFIG();
        return cfg;
    }

    namespace detail
    {
        struct Callbacks
        {
            TextCallback on_text;
            StateCallback on_state;
        };

        // Only one listening pipeline can exist, so a single set of callbacks suffices.
        inline Callbacks &GetCallbacks(void)
        {
            static Callbacks callbacks;
            return callbacks;
        }

        inline esp_transcribe_config_t Bind(esp_transcribe_config_t cfg, TextCallback on_text, StateCallback on_state)
        {
            GetCallbacks() = {std::move(on_text), std::move(on_state)};
            cfg.on_text = [](const char *text, void *) { GetCallbacks().on_text(text); };
            cfg.on_state = [](esp_transcribe_state_t state, void *) {
                if (GetCallbacks().on_state)
                {
                    GetCallbacks().on_state(state);
                }
            };
            cfg.user_ctx = nullptr;
            return cfg;
        }
    } // namespace detail

    /** Loads the model early. Optional. */
    inline esp_err_t Init(void) { return esp_transcribe_init(); }

    /** Starts the always-listening pipeline on an I2S microphone. */
    inline esp_err_t StartMic(TextCallback on_text, const esp_transcribe_config_t &cfg = DefaultConfig(),
                              StateCallback on_state = {})
    {
        if (!on_text)
        {
            return ESP_ERR_INVALID_ARG;
        }
        const esp_transcribe_config_t bound = detail::Bind(cfg, std::move(on_text), std::move(on_state));
        return esp_transcribe_start_mic(&bound);
    }

    /** Starts the always-listening pipeline on an esp_codec_dev record device. */
    inline esp_err_t StartCodecDev(const esp_transcribe_codec_dev_config_t &dev, TextCallback on_text,
                                   const esp_transcribe_config_t &cfg = DefaultConfig(), StateCallback on_state = {})
    {
        if (!on_text)
        {
            return ESP_ERR_INVALID_ARG;
        }
        const esp_transcribe_config_t bound = detail::Bind(cfg, std::move(on_text), std::move(on_state));
        return esp_transcribe_start_codec_dev(&dev, &bound);
    }

    /** Starts the always-listening pipeline on audio pushed with Feed(). */
    inline esp_err_t StartStream(TextCallback on_text, const esp_transcribe_config_t &cfg = DefaultConfig(),
                                 StateCallback on_state = {})
    {
        if (!on_text)
        {
            return ESP_ERR_INVALID_ARG;
        }
        const esp_transcribe_config_t bound = detail::Bind(cfg, std::move(on_text), std::move(on_state));
        return esp_transcribe_start_stream(&bound);
    }

    /** Pushes 16kHz mono int16 audio into the stream pipeline. Returns the number of samples accepted. */
    inline size_t Feed(const int16_t *pcm, size_t n_samples, uint32_t timeout = UINT32_MAX)
    {
        return esp_transcribe_feed(pcm, n_samples, timeout);
    }

    /** Transcribes a buffer of 16kHz mono int16 audio (at most ESP_TRANSCRIBE_MAX_SAMPLES). */
    inline esp_err_t Transcribe(const int16_t *pcm, size_t n_samples, std::string &text)
    {
        std::string buffer(512, '\0');
        const esp_err_t err = esp_transcribe_run(pcm, n_samples, buffer.data(), buffer.size());
        if (err == ESP_OK)
        {
            buffer.resize(std::char_traits<char>::length(buffer.c_str()));
            text = std::move(buffer);
        }
        return err;
    }

} // namespace esp_transcribe
