#ifndef AUDIO_SOURCE_H_
#define AUDIO_SOURCE_H_

#include <cstddef>
#include <cstdint>

#include <tlib_tensor.h>

/**
 * Source of 16kHz mono int16 audio for the recording task.
 */
class AudioSource
{
public:
    virtual ~AudioSource(void) = default;

    /**
     * Blocks until kChunkSize samples are read into the given buffer.
     *
     * @param   buffer  [kChunkSize*2] int16_t tensor. The second half may be used as scratch space.
     *
     * @return  The number of valid samples written to the buffer.
     */
    virtual size_t Read(tlib::Tensor<int16_t> &buffer) = 0;
};

#endif
