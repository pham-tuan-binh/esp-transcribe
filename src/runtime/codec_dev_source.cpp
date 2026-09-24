#include "codec_dev_source.h"

#include <algorithm>

#include <esp_log.h>

#include <config.h>

static const char *kTag{"esp_transcribe"};

// Frames read per call for multi-channel devices (20ms)
static constexpr size_t kBlockFrames{320};

CodecDevSource::CodecDevSource(esp_codec_dev_handle_t handle, const uint8_t channels, const uint8_t channel)
    : handle_{handle}, channels_{channels}, channel_{channel}
{
    if (channels_ > 1)
    {
        scratch_.resize(kBlockFrames * channels_);
    }
}

size_t CodecDevSource::Read(tlib::Tensor<int16_t> &buffer)
{
    int16_t *data{buffer.Data()};

    if (channels_ == 1)
    {
        if (esp_codec_dev_read(handle_, data, kChunkSize * sizeof(int16_t)) != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(kTag, "Error reading from codec device");
            return 0;
        }
        return kChunkSize;
    }

    for (size_t frame = 0; frame < kChunkSize; frame += kBlockFrames)
    {
        const size_t n_frames{std::min<size_t>(kBlockFrames, kChunkSize - frame)};
        if (esp_codec_dev_read(handle_, scratch_.data(), n_frames * channels_ * sizeof(int16_t)) != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(kTag, "Error reading from codec device");
            return frame;
        }
        for (size_t i = 0; i < n_frames; i++)
        {
            data[frame + i] = scratch_[i * channels_ + channel_];
        }
    }
    return kChunkSize;
}
