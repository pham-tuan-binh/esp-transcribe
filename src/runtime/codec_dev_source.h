#ifndef CODEC_DEV_SOURCE_H_
#define CODEC_DEV_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include <esp_codec_dev.h>

#include <audio_source.h>

/**
 * Audio source reading from an esp_codec_dev record device. Multi-channel devices
 * are read interleaved and a single channel is extracted.
 */
class CodecDevSource : public AudioSource
{
public:
    /**
     * @param   handle      Opened record device, delivering 16kHz 16-bit audio
     * @param   channels    Number of interleaved channels delivered by the device
     * @param   channel     Index of the channel to extract
     */
    explicit CodecDevSource(esp_codec_dev_handle_t handle, const uint8_t channels, const uint8_t channel);

    /**
     * Delete move and copy constructors/assignments
     */
    CodecDevSource(const CodecDevSource &) = delete;
    CodecDevSource(const CodecDevSource &&) = delete;
    CodecDevSource &operator=(const CodecDevSource &) = delete;
    CodecDevSource &&operator=(const CodecDevSource &&) = delete;

    size_t Read(tlib::Tensor<int16_t> &buffer) override;

private:
    const esp_codec_dev_handle_t handle_; /*!< Record device */
    const uint8_t channels_;              /*!< Interleaved channels delivered by the device */
    const uint8_t channel_;               /*!< Channel to extract */
    std::vector<int16_t> scratch_;        /*!< Interleaved read buffer for multi-channel devices */
};

#endif
