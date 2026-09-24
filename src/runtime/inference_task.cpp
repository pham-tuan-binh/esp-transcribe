#include "inference_task.h"

#include <chrono>
#include <cstdlib>

#include <freertos/FreeRTOS.h>
#include <esp_log.h>

#include <conformer_preprocessing.h>
#include <conformer_pre_encode.h>
#include <conformer_tokenizer.h>
#include <conformer.h>

#include <tlib_ops.h>
#include <tlib_linear_shared.h>

#include <config.h>

static const char *kTag{"esp_transcribe"};

std::string TranscribeChunks(std::vector<tlib::Tensor<float>> &pre_enc_chunks)
{
    assert(!pre_enc_chunks.empty() && pre_enc_chunks.size() <= kMaxChunks);

    tlib::ops::shared::EnableSharedExecution();
    const uint32_t audio_len_ms = (kChunkSize * pre_enc_chunks.size() * 1'000U) / kSamplingRate;
    const auto t_start = std::chrono::high_resolution_clock::now();
    auto x = tlib::ops::cat(pre_enc_chunks);
    pre_enc_chunks.clear();
    x = conformer::Forward(x);
    const tlib::Tensor<uint32_t> tokens{tlib::ops::argmax(x)};
    std::string transcription;
    conformer::Decode(tokens, transcription);
    const auto t_end = std::chrono::high_resolution_clock::now();
    tlib::ops::shared::DisableSharedExecution();

    const auto inference_time = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start);
    ESP_LOGI(kTag, "Transcribed %ums of audio in %lldms", static_cast<unsigned>(audio_len_ms),
             static_cast<long long>(inference_time.count()));
    return transcription;
}

InferenceTask::InferenceTask(
    const std::shared_ptr<std::array<tlib::Tensor<int16_t>, 2>> recording_buffers,
    const esp_transcribe_config_t &config,
    const SemaphoreHandle_t chunk_consumed)
    : Runner(), recording_buffers_{recording_buffers}, config_{config}, chunk_consumed_{chunk_consumed}
{
    assert(recording_buffers_->at(0).Numel() == kChunkSize*2);
    assert(recording_buffers_->at(1).Numel() == kChunkSize*2);

    pre_enc_chunks_.reserve(config_.vad.max_chunks);
}

void InferenceTask::Update(void)
{
    // Wait for recording task to provide the next audio chunk
    uint32_t audio_buffer_index{};
    xTaskNotifyWait(0, 0, &audio_buffer_index, portMAX_DELAY);
    const auto &audio_chunk{recording_buffers_->at(audio_buffer_index)};

    // Check if this audio chunk contains significant sound
    const bool contains_sound{DetectSound(audio_chunk)};
    if (contains_sound)
    {
        non_slient_chunks_++;
        consecutive_silence_chunks_ = 0;
    }
    else
    {
        consecutive_silence_chunks_++;
    }
    auto enc_chunk = conformer::PreEncode(preprocessor_.Forward(audio_chunk, kChunkSize));

    // The recording buffer is not needed anymore
    if (chunk_consumed_ != nullptr)
    {
        xSemaphoreGive(chunk_consumed_);
    }

    if (pre_enc_chunks_.size() == 0)
    {
        pre_enc_chunks_.push_back(std::move(enc_chunk));
        return;
    }

    // Always keep at least one silent chunk, since it may partially contain voice
    if (pre_enc_chunks_.size() == 1 && non_slient_chunks_ == 0 && !contains_sound)
    {
        pre_enc_chunks_.at(0) = std::move(enc_chunk);
        consecutive_silence_chunks_ = 1;
        return;
    }

    if (contains_sound && non_slient_chunks_ == 1)
    {
        ESP_LOGD(kTag, "Listening...");
        SetState(ESP_TRANSCRIBE_STATE_LISTENING);
    }

    pre_enc_chunks_.push_back(std::move(enc_chunk));

    // Did we reach maximum number of chunks?
    if (pre_enc_chunks_.size() >= config_.vad.max_chunks)
    {
        RunInference();
        return;
    }

    // Did we reach the end of sentence?
    if (consecutive_silence_chunks_ >= config_.vad.eos_chunks)
    {

        // Are there enough non-silent chunks recorded?
        if (non_slient_chunks_ < config_.vad.min_chunks)
        {
            ESP_LOGD(kTag, "Not enough non-silent chunks.");
            Reset();
            SetState(ESP_TRANSCRIBE_STATE_IDLE);
            return;
        }

        // Prune recording if enabled
        if (config_.vad.prune_chunks > 0) {
            assert(pre_enc_chunks_.size() > config_.vad.prune_chunks);
            pre_enc_chunks_.erase(
                pre_enc_chunks_.end() - config_.vad.prune_chunks,
                pre_enc_chunks_.end());
        }
        RunInference();
        return;
    }
}

bool InferenceTask::DetectSound(const tlib::TensorView<int16_t> &recording_buffer) const
{
    uint32_t sum{0};
    const int16_t *buffer_data{recording_buffer.DataImm()};

    for (uint32_t i = 0; i < kChunkSize; i++)
    {
        sum += std::abs(buffer_data[i]);
    }

    const uint32_t avg_val{sum / kChunkSize};
    return avg_val > config_.vad.threshold;
}

void InferenceTask::RunInference(void)
{
    SetState(ESP_TRANSCRIBE_STATE_TRANSCRIBING);
    const std::string transcription{TranscribeChunks(pre_enc_chunks_)};
    Reset();
    if (config_.on_text != nullptr)
    {
        config_.on_text(transcription.c_str(), config_.user_ctx);
    }
    SetState(ESP_TRANSCRIBE_STATE_IDLE);
}

void InferenceTask::Reset(void)
{
    pre_enc_chunks_.clear();
    consecutive_silence_chunks_ = 0;
    non_slient_chunks_ = 0;
}

void InferenceTask::SetState(const esp_transcribe_state_t new_state)
{
    if (config_.on_state != nullptr)
    {
        config_.on_state(new_state, config_.user_ctx);
    }
}
