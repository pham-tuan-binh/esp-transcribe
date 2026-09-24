#ifndef RECORDING_TASK_H_
#define RECORDING_TASK_H_

#include <array>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <runner.h>
#include <audio_source.h>
#include <tlib_tensor.h>

/**
 * The recording task records audio from an audio source (e.g. an I2S microphone) into a
 * switching buffer and sends the result to the inference task.
 */
class RecordingTask : public Runner
{
public:
    /**
     * Initializes the recording task.
     *
     * @param   recording_buffers       Pointer to pair of tensors which are used as recording buffers.
     *                                  Each recording buffer needs to have a size of kChunkSize*2.
     * @param   inference_task_handle   Handle of inference task to notify it once a recording buffer
     *                                  is ready.
     * @param   source                  Source to record audio from.
     * @param   chunk_consumed          Optional semaphore that the inference task gives once it is
     *                                  done with a recording buffer. If given, the recording task
     *                                  waits for it before reusing a buffer. Needed for sources that
     *                                  can deliver audio faster than real time.
     */
    explicit RecordingTask(
        const std::shared_ptr<std::array<tlib::Tensor<int16_t>, 2>> recording_buffers,
        const TaskHandle_t inference_task_handle,
        std::unique_ptr<AudioSource> source,
        const SemaphoreHandle_t chunk_consumed);

    /**
     * Delete move and copy constructors/assignments
     */
    RecordingTask(const RecordingTask &) = delete;
    RecordingTask(const RecordingTask &&) = delete;
    RecordingTask &operator=(const RecordingTask &) = delete;
    RecordingTask &&operator=(const RecordingTask &&) = delete;

    /**
     * Task loop of the recording task. Does the following:
     * 1. Switch active recording buffer
     * 2. Record kChunkSize audio samples into the active buffer
     * 3. Notify inference task that recording buffer is ready
     */
    void Update(void) override;

private:
    const std::shared_ptr<std::array<tlib::Tensor<int16_t>, 2>> recording_buffers_; /*!< Pointer to recording buffers */
    const TaskHandle_t inference_task_handle_;                                      /*!< Handle of inference task for notifications */
    const std::unique_ptr<AudioSource> source_;                                     /*!< Source to record audio from */
    const SemaphoreHandle_t chunk_consumed_;                                        /*!< Given by the inference task once a buffer is processed */
    bool awaiting_consumption_{false};                                              /*!< Flag to indicate that the inference task still processes a buffer */
    uint8_t active_buffer_index_{0};                                                /*!< Index of currently active buffer */
    bool inference_task_was_previously_busy_{true};                                 /*!< Flag to indicate if the inference task was performing inference during the last update cycle */
};

#endif