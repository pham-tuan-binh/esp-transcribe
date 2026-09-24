#ifndef STREAM_SOURCE_H_
#define STREAM_SOURCE_H_

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

#include <audio_source.h>

/**
 * Audio source that is fed by the application through Write, e.g. from a network stream.
 * Write and Read may each only be called from a single task.
 */
class StreamSource : public AudioSource
{
public:
    /**
     * Allocates a stream buffer in PSRAM that can hold two chunks of audio.
     */
    explicit StreamSource(void);
    ~StreamSource(void) override;

    /**
     * Delete move and copy constructors/assignments
     */
    StreamSource(const StreamSource &) = delete;
    StreamSource(const StreamSource &&) = delete;
    StreamSource &operator=(const StreamSource &) = delete;
    StreamSource &&operator=(const StreamSource &&) = delete;

    /**
     * @return  True, if the stream buffer was allocated successfully.
     */
    bool IsValid(void) const { return stream_buffer_ != nullptr; }

    /**
     * Appends samples to the stream.
     *
     * @param   samples     Audio samples
     * @param   n_samples   Number of samples
     * @param   timeout     Ticks to wait for buffer space
     *
     * @return  The number of samples written.
     */
    size_t Write(const int16_t *samples, size_t n_samples, TickType_t timeout);

    size_t Read(tlib::Tensor<int16_t> &buffer) override;

private:
    uint8_t *storage_{nullptr};                 /*!< Stream buffer storage */
    StaticStreamBuffer_t stream_buffer_struct_; /*!< Stream buffer control block */
    StreamBufferHandle_t stream_buffer_{nullptr};
};

#endif
