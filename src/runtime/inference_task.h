#ifndef INFERENCE_TASK_H_
#define INFERENCE_TASK_H_

#include <array>
#include <string>
#include <vector>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_transcribe.h>

#include <runner.h>
#include <tlib_tensor.h>
#include <conformer_preprocessing.h>

/**
 * Runs the conformer on a list of preprocessed chunks (outputs of PreEncode) and decodes the result.
 * Clears the given chunks. Shared execution with the co inference task is enabled for the duration
 * of the forward pass.
 *
 * @param   pre_enc_chunks  Preprocessed chunks, at most kMaxChunks.
 *
 * @return  The transcription.
 */
std::string TranscribeChunks(std::vector<tlib::Tensor<float>> &pre_enc_chunks);

/**
 * Main inference worker, which preprocesses audio chunks and triggers inference if required.
 */
class InferenceTask : public Runner
{
public:
    /**
     * Initializes the inference task.
     *
     * @param   recording_buffers   Pointer to pair of tensors which are used as recording buffers.
     *                              Each recording buffer needs to have a size of kChunkSize*2.
     * @param   config              Voice activity detection parameters and callbacks.
     * @param   chunk_consumed      Optional semaphore which is given once a recording buffer is processed.
     */
    explicit InferenceTask(
        const std::shared_ptr<std::array<tlib::Tensor<int16_t>, 2>> recording_buffers,
        const esp_transcribe_config_t &config,
        const SemaphoreHandle_t chunk_consumed);

    /**
     * Delete move and copy constructors/assignments
     */
    InferenceTask(const InferenceTask &) = delete;
    InferenceTask(const InferenceTask &&) = delete;
    InferenceTask &operator=(const InferenceTask &) = delete;
    InferenceTask &&operator=(const InferenceTask &&) = delete;

    /**
     * Task loop of the inference task. Does the following:
     * 1. Wait until the next recording buffer is ready
     * 2. Preprocesses the recording buffer and stores the resulting chunk
     * 3. Performs inference if enough chunks were recorded
     * 4. Resets the chunk buffer after inference
     */
    void Update(void) override;

private:
    /**
     * Determines if the given recording_buffer is silent or not. To detect sound,
     * the mean over the all absolute values in the given buffer is calculated
     * and compared against the configured threshold. If the mean is below the threshold,
     * the audio is considered to be silent.
     *
     * @param   recording_buffer    [n] int16_t tensor, audio
     *
     * @return  True, if the audio is not silent, false otherwise.
     */
    bool DetectSound(const tlib::TensorView<int16_t> &recording_buffer) const;

    /**
     * Launches model inference on all stored chunks.
     */
    void RunInference(void);

    /**
     * Clears the stored chunks and resets internal counters.
     */
    void Reset(void);

    /**
     * Reports a state change to the state callback, if any.
     *
     * @param   new_state   The new pipeline state.
     */
    void SetState(const esp_transcribe_state_t new_state);

    const std::shared_ptr<std::array<tlib::Tensor<int16_t>, 2>> recording_buffers_; /*!< Pointer to recording buffers */
    const esp_transcribe_config_t config_;                                          /*!< VAD parameters and callbacks */
    const SemaphoreHandle_t chunk_consumed_;                                        /*!< Given once a recording buffer is processed */
    std::vector<tlib::Tensor<float>> pre_enc_chunks_;                               /*!< Chunk storage */
    conformer::Preprocessor preprocessor_{};                                        /*!< Preprocessor for the conformer model */
    uint8_t consecutive_silence_chunks_{0};                                         /*!< Number of consecutive chunks with silence */
    uint8_t non_slient_chunks_{0};                                                  /*!< Number of non-silent chunks in the chunk storage */
};

#endif
