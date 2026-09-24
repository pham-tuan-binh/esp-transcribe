#include "recording_task.h"

#include <chrono>

#include <esp_log.h>

#include <config.h>
#include <tlib_linear_shared.h>

RecordingTask::RecordingTask(
    const std::shared_ptr<std::array<tlib::Tensor<int16_t>, 2>> recording_buffers,
    const TaskHandle_t inference_task_handle,
    std::unique_ptr<AudioSource> source,
    const SemaphoreHandle_t chunk_consumed)
    : Runner(), recording_buffers_{recording_buffers}, inference_task_handle_{inference_task_handle},
      source_{std::move(source)}, chunk_consumed_{chunk_consumed}
{
    assert(recording_buffers_->at(0).Numel() == kChunkSize * 2);
    assert(recording_buffers_->at(1).Numel() == kChunkSize * 2);
}

static const char *kTag{"esp_transcribe"};

void RecordingTask::Update(void)
{
    // Do not overwrite a buffer that the inference task may still be reading
    if (awaiting_consumption_)
    {
        xSemaphoreTake(chunk_consumed_, portMAX_DELAY);
        awaiting_consumption_ = false;
    }

    active_buffer_index_ = (active_buffer_index_ + 1) % recording_buffers_->size();

    const auto n_read = source_->Read(recording_buffers_->at(active_buffer_index_));

    // If we did not read enough samples, the recording task was probably interrupted by some other
    // task running on the core.
    if (n_read != kChunkSize)
    {
        ESP_LOGW(kTag, "Could only record %u/%u samples", static_cast<unsigned>(n_read), static_cast<unsigned>(kChunkSize));
    }

    // If shared execution is enabled, the inference task is performing inference and can't process
    // new audio chunks. In that case, we do not need to notify it. The purpose of the previously
    // busy flag is to skip another chunk after the inference task is finished. The current audio
    // chunk may have partially been recorded while inference was still running. In this case, audio
    // is potentially corrupted as the CoInference task can interrupt the recording task at any time.
    if (tlib::ops::shared::IsSharedExecutionEnabled())
    {
        inference_task_was_previously_busy_ = true;
        return;
    }
    else if (inference_task_was_previously_busy_)
    {
        inference_task_was_previously_busy_ = false;
    }
    else
    {
        // Inform the inference task that the next audio chunk is ready for processing.
        if (xTaskNotify(inference_task_handle_, active_buffer_index_, eSetValueWithOverwrite) == pdFALSE)
        {
            ESP_LOGW(kTag, "Failed to notify inference task");
        }
        else if (chunk_consumed_ != nullptr)
        {
            awaiting_consumption_ = true;
        }
    }
}