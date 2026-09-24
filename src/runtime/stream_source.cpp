#include "stream_source.h"

#include <esp_heap_caps.h>

#include <config.h>

static constexpr size_t kStreamBufferBytes{kChunkSize * 2 * sizeof(int16_t)};

StreamSource::StreamSource(void)
{
    // FreeRTOS requires one byte of storage beyond the buffer size
    storage_ = static_cast<uint8_t *>(heap_caps_malloc(kStreamBufferBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (storage_ != nullptr)
    {
        stream_buffer_ = xStreamBufferCreateStatic(kStreamBufferBytes, sizeof(int16_t), storage_, &stream_buffer_struct_);
    }
}

StreamSource::~StreamSource(void)
{
    if (stream_buffer_ != nullptr)
    {
        vStreamBufferDelete(stream_buffer_);
    }
    heap_caps_free(storage_);
}

size_t StreamSource::Write(const int16_t *samples, size_t n_samples, TickType_t timeout)
{
    // Only whole samples are ever written and read, so byte counts in the buffer stay even.
    const uint8_t *data{reinterpret_cast<const uint8_t *>(samples)};
    const size_t total_bytes{n_samples * sizeof(int16_t)};
    size_t written_bytes{0};
    TimeOut_t time_out;
    vTaskSetTimeOutState(&time_out);
    while (written_bytes < total_bytes)
    {
        written_bytes += xStreamBufferSend(stream_buffer_, data + written_bytes, total_bytes - written_bytes, timeout);
        if (written_bytes < total_bytes && xTaskCheckForTimeOut(&time_out, &timeout) == pdTRUE)
        {
            break;
        }
    }
    return written_bytes / sizeof(int16_t);
}

size_t StreamSource::Read(tlib::Tensor<int16_t> &buffer)
{
    uint8_t *data{reinterpret_cast<uint8_t *>(buffer.Data())};
    const size_t total_bytes{kChunkSize * sizeof(int16_t)};
    size_t read_bytes{0};
    while (read_bytes < total_bytes)
    {
        read_bytes += xStreamBufferReceive(stream_buffer_, data + read_bytes, total_bytes - read_bytes, portMAX_DELAY);
    }
    return kChunkSize;
}
