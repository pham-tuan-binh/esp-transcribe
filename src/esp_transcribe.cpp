#include "esp_transcribe.h"
#include "esp_transcribe_codec_dev.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sdkconfig.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <config.h>
#include <tlib_heap.h>
#include <tlib_flash.h>
#include <conformer_preprocessing.h>
#include <conformer_pre_encode.h>
#include <flash_callback.h>
#include <co_inference_task.h>
#include <inference_task.h>
#include <recording_task.h>
#include <microphone.h>
#include <stream_source.h>
#include <codec_dev_source.h>

#if !CONFIG_SPIRAM
#error "esp-transcribe needs PSRAM: enable CONFIG_SPIRAM (Component config -> ESP PSRAM)"
#endif

static_assert(ESP_TRANSCRIBE_SAMPLE_RATE == kSamplingRate);
static_assert(ESP_TRANSCRIBE_CHUNK_SAMPLES == kChunkSize);
static_assert(ESP_TRANSCRIBE_MAX_CHUNKS == kMaxChunks);

namespace
{

    const char *kTag{"esp_transcribe"};

    // The SRAM pool has to be one contiguous block of internal RAM, which is only guaranteed statically.
    // Zero-initialized, so it lands in internal .bss (not in the app binary).
    uint8_t __attribute__((aligned(64))) heap_sram[kSRAMHeapSize];
    uint8_t *heap_psram{nullptr};

    std::mutex mutex;
    bool initialized{false};
    esp_err_t init_error{ESP_OK};
    bool pipeline_started{false};

    std::unique_ptr<CoInferenceTask> co_inference_task;
    std::unique_ptr<InferenceTask> inference_task;
    std::unique_ptr<RecordingTask> recording_task;
    std::shared_ptr<std::array<tlib::Tensor<int16_t>, 2>> recording_buffers;
    std::atomic<StreamSource *> stream_source{nullptr}; // Owned by recording_task
    std::unique_ptr<conformer::Preprocessor> run_preprocessor; // Reused by esp_transcribe_run

    esp_err_t InitLocked(void)
    {
        if (initialized)
        {
            return init_error;
        }
        initialized = true;

        const esp_partition_t *partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, CONFIG_ESP_TRANSCRIBE_MODEL_PARTITION);
        if (partition == nullptr)
        {
            ESP_LOGE(kTag, "No data partition named '%s'. Add it to your partition table (see README).",
                     CONFIG_ESP_TRANSCRIBE_MODEL_PARTITION);
            return init_error = ESP_ERR_NOT_FOUND;
        }

        heap_psram = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, kPSRAMHeapSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (heap_psram == nullptr)
        {
            ESP_LOGE(kTag, "Could not allocate %u bytes of PSRAM", static_cast<unsigned>(kPSRAMHeapSize));
            return init_error = ESP_ERR_NO_MEM;
        }
        tlib::heap::Initialize(heap_sram, kSRAMHeapSize, heap_psram, kPSRAMHeapSize);

        tlib::flash::SetModelFlashAddress(partition->address);
        const auto flash_error = tlib::flash::Initialize(partition->address, tlib::flash::OnFlashReadRequest);
        if (flash_error != tlib::flash::FlashError::None)
        {
            ESP_LOGE(kTag, "No valid model in partition '%s' (error %d). Flash it with `idf.py flash` "
                           "or `idf.py %s-flash`.",
                     partition->label, static_cast<int>(flash_error), partition->label);
            return init_error = ESP_ERR_INVALID_STATE;
        }

        // Without the co inference task, all matrix multiplications run on the inference core only.
        if (kDualCore)
        {
            co_inference_task = std::make_unique<CoInferenceTask>();
            co_inference_task->Start("transcribe_co", kCoInferenceTaskStackSize, kCoInferenceTaskPriority, kCoInferenceTaskCore);
        }

        ESP_LOGI(kTag, "Model loaded from partition '%s' at 0x%" PRIx32 " (%s)", partition->label, partition->address,
                 kDualCore ? "dual core" : "single core");
        return init_error = ESP_OK;
    }

    esp_err_t ValidateConfig(const esp_transcribe_config_t *cfg)
    {
        if (cfg == nullptr || cfg->on_text == nullptr)
        {
            ESP_LOGE(kTag, "Config and on_text callback are required");
            return ESP_ERR_INVALID_ARG;
        }
        const auto &vad{cfg->vad};
        if (vad.max_chunks == 0 || vad.max_chunks > kMaxChunks || vad.min_chunks > vad.max_chunks ||
            vad.eos_chunks < 2 || vad.prune_chunks >= vad.eos_chunks || cfg->mic.gain_shift > 31)
        {
            ESP_LOGE(kTag, "Invalid config: need 0 < max_chunks <= %u, min_chunks <= max_chunks, "
                           "eos_chunks > 1, prune_chunks < eos_chunks, gain_shift <= 31",
                     static_cast<unsigned>(kMaxChunks));
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }

    /**
     * Starts the listening pipeline. Must be called with the mutex held and after InitLocked.
     *
     * @param   cfg             Validated configuration
     * @param   source          Audio source for the recording task
     * @param   paced_by_source True if the source can deliver audio faster than real time, which
     *                          requires the recording task to wait for the inference task.
     */
    esp_err_t StartPipelineLocked(const esp_transcribe_config_t &cfg, std::unique_ptr<AudioSource> source,
                                  const bool paced_by_source)
    {
        SemaphoreHandle_t chunk_consumed{nullptr};
        if (paced_by_source)
        {
            chunk_consumed = xSemaphoreCreateBinary();
            if (chunk_consumed == nullptr)
            {
                return ESP_ERR_NO_MEM;
            }
        }

        recording_buffers = std::make_shared<std::array<tlib::Tensor<int16_t>, 2>>(
            std::array<tlib::Tensor<int16_t>, 2>({
                tlib::Tensor<int16_t>({kChunkSize * 2}, false, 0),
                tlib::Tensor<int16_t>({kChunkSize * 2}, false, 0)
            })
        );

        // The pipeline has its own preprocessor, esp_transcribe_run is unavailable from now on.
        run_preprocessor.reset();

        inference_task = std::make_unique<InferenceTask>(recording_buffers, cfg, chunk_consumed);
        const TaskHandle_t inference_task_handle = inference_task->Start(
            "transcribe_inf", kInferenceTaskStackSize, kInferenceTaskPriority, kInferenceTaskCore);
        recording_task = std::make_unique<RecordingTask>(recording_buffers, inference_task_handle, std::move(source), chunk_consumed);
        recording_task->Start("transcribe_rec", kRecordingTaskStackSize, kRecordingTaskPriority, kRecordingTaskCore);

        pipeline_started = true;
        return ESP_OK;
    }

    // Runs fn on a new task on the inference core, so that it is on a different core than the
    // co inference task, and waits for it. Anything fn allocates must be freed inside fn.
    esp_err_t RunOnInferenceCore(const std::function<void(void)> &fn)
    {
        struct Job
        {
            const std::function<void(void)> *fn;
            SemaphoreHandle_t done;
        } job{&fn, xSemaphoreCreateBinary()};
        if (job.done == nullptr)
        {
            return ESP_ERR_NO_MEM;
        }
        auto task = [](void *arg) {
            Job *job{static_cast<Job *>(arg)};
            (*job->fn)();
            xSemaphoreGive(job->done);
            vTaskDelete(nullptr);
        };
        if (xTaskCreatePinnedToCore(task, "transcribe_run", kInferenceTaskStackSize, &job,
                                    kInferenceTaskPriority, nullptr, kInferenceTaskCore) != pdPASS)
        {
            vSemaphoreDelete(job.done);
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreTake(job.done, portMAX_DELAY);
        vSemaphoreDelete(job.done);
        return ESP_OK;
    }

    // The preprocessor's window and FFT tables are computed once and kept for later calls
    conformer::Preprocessor &RunPreprocessor(void)
    {
        if (!run_preprocessor)
        {
            run_preprocessor = std::make_unique<conformer::Preprocessor>();
        }
        return *run_preprocessor;
    }

    // Incremental transcription (esp_transcribe_begin/push/finish). Audio is split into
    // independent chunks, so each chunk is preprocessed and pre-encoded on a worker task as
    // soon as it is complete, while the caller is still recording. Same result as
    // esp_transcribe_run() on the whole buffer.
    struct Session
    {
        std::array<int16_t *, 2> buffers{}; // Filled by push, handed to the worker in turn
        uint8_t current{0};                 // Buffer that push is filling
        size_t fill{0};                     // Samples in the current buffer
        size_t total{0};                    // Samples pushed so far
        QueueHandle_t queue{nullptr};       // Buffer index to pre-encode, or kSessionEnd
        SemaphoreHandle_t free_buffers{nullptr};
        SemaphoreHandle_t done{nullptr};
        std::vector<tlib::Tensor<float>> pre_enc_chunks;
        tlib::Tensor<int16_t> chunk; // Worker's input chunk

        ~Session()
        {
            for (int16_t *buffer : buffers)
            {
                heap_caps_free(buffer);
            }
            if (queue != nullptr)
            {
                vQueueDelete(queue);
            }
            if (free_buffers != nullptr)
            {
                vSemaphoreDelete(free_buffers);
            }
            if (done != nullptr)
            {
                vSemaphoreDelete(done);
            }
        }
    };
    constexpr int kSessionEnd{-1};
    std::unique_ptr<Session> session;

    void SessionTask(void *arg)
    {
        Session *s{static_cast<Session *>(arg)};
        {
            int index{};
            while (xQueueReceive(s->queue, &index, portMAX_DELAY) == pdTRUE && index != kSessionEnd)
            {
                std::memcpy(s->chunk.Data(), s->buffers[index], kChunkSize * sizeof(int16_t));
                xSemaphoreGive(s->free_buffers);
                s->pre_enc_chunks.push_back(conformer::PreEncode(RunPreprocessor().Forward(s->chunk, kChunkSize)));
            }
        }
        xSemaphoreGive(s->done);
        vTaskDelete(nullptr);
    }

    // Stops the worker once it has pre-encoded everything queued
    void StopSessionTask(Session &s)
    {
        xQueueSend(s.queue, &kSessionEnd, portMAX_DELAY);
        xSemaphoreTake(s.done, portMAX_DELAY);
    }

} // namespace

extern "C" esp_err_t esp_transcribe_init(void)
{
    std::lock_guard lock{mutex};
    return InitLocked();
}

extern "C" esp_err_t esp_transcribe_start_mic(const esp_transcribe_config_t *cfg)
{
    const esp_err_t err = ValidateConfig(cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    std::lock_guard lock{mutex};
    if (pipeline_started || session)
    {
        ESP_LOGE(kTag, "Listening pipeline already running");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(InitLocked(), kTag, "Init failed");

    auto microphone = std::make_unique<Microphone>(
        cfg->mic.port, static_cast<gpio_num_t>(cfg->mic.bclk_gpio), static_cast<gpio_num_t>(cfg->mic.ws_gpio),
        static_cast<gpio_num_t>(cfg->mic.din_gpio), cfg->mic.gain_shift);
    return StartPipelineLocked(*cfg, std::move(microphone), false);
}

extern "C" esp_err_t esp_transcribe_start_stream(const esp_transcribe_config_t *cfg)
{
    const esp_err_t err = ValidateConfig(cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    std::lock_guard lock{mutex};
    if (pipeline_started || session)
    {
        ESP_LOGE(kTag, "Listening pipeline already running");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(InitLocked(), kTag, "Init failed");

    auto source = std::make_unique<StreamSource>();
    if (!source->IsValid())
    {
        return ESP_ERR_NO_MEM;
    }
    StreamSource *source_ptr{source.get()};
    ESP_RETURN_ON_ERROR(StartPipelineLocked(*cfg, std::move(source), true), kTag, "Start failed");
    stream_source = source_ptr;
    return ESP_OK;
}

extern "C" esp_err_t esp_transcribe_start_codec_dev(const esp_transcribe_codec_dev_config_t *dev,
                                                    const esp_transcribe_config_t *cfg)
{
    const esp_err_t err = ValidateConfig(cfg);
    if (err != ESP_OK)
    {
        return err;
    }
    if (dev == nullptr || dev->handle == nullptr || dev->channels == 0 || dev->channel >= dev->channels)
    {
        ESP_LOGE(kTag, "Invalid codec device config: need a handle and channel < channels");
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard lock{mutex};
    if (pipeline_started || session)
    {
        ESP_LOGE(kTag, "Listening pipeline already running");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(InitLocked(), kTag, "Init failed");

    if (!dev->skip_open)
    {
        esp_codec_dev_sample_info_t sample_info{};
        sample_info.bits_per_sample = 16;
        sample_info.channel = dev->channels;
        sample_info.channel_mask = dev->channel_mask;
        sample_info.sample_rate = kSamplingRate;
        const int res = esp_codec_dev_open(dev->handle, &sample_info);
        if (res != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(kTag, "Could not open codec device at 16kHz/16-bit/%u channels (error %d)",
                     static_cast<unsigned>(dev->channels), res);
            return ESP_FAIL;
        }
    }

    auto source = std::make_unique<CodecDevSource>(dev->handle, dev->channels, dev->channel);
    return StartPipelineLocked(*cfg, std::move(source), false);
}

extern "C" size_t esp_transcribe_feed(const int16_t *pcm, size_t n_samples, uint32_t timeout)
{
    // stream_source is only set once and never cleared, so no lock is needed here
    StreamSource *source{stream_source.load()};
    if (source == nullptr || pcm == nullptr)
    {
        return 0;
    }
    return source->Write(pcm, n_samples, timeout);
}

extern "C" esp_err_t esp_transcribe_run(const int16_t *pcm, size_t n_samples, char *text, size_t text_size)
{
    if (pcm == nullptr || text == nullptr || text_size == 0 || n_samples == 0 || n_samples > ESP_TRANSCRIBE_MAX_SAMPLES)
    {
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard lock{mutex};
    if (pipeline_started || session)
    {
        ESP_LOGE(kTag, "esp_transcribe_run is not available while a listening pipeline or session is running");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(InitLocked(), kTag, "Init failed");

    std::string transcription;
    ESP_RETURN_ON_ERROR(RunOnInferenceCore([&] {
        tlib::Tensor<int16_t> chunk({kChunkSize}, false, 0);
        std::vector<tlib::Tensor<float>> pre_enc_chunks;
        pre_enc_chunks.reserve(kMaxChunks);

        for (size_t offset = 0; offset < n_samples; offset += kChunkSize)
        {
            const size_t n = std::min<size_t>(kChunkSize, n_samples - offset);
            std::memcpy(chunk.Data(), pcm + offset, n * sizeof(int16_t));
            std::memset(chunk.Data() + n, 0, (kChunkSize - n) * sizeof(int16_t));
            pre_enc_chunks.push_back(conformer::PreEncode(RunPreprocessor().Forward(chunk, kChunkSize)));
        }
        transcription = TranscribeChunks(pre_enc_chunks);
    }), kTag, "Could not start the inference task");

    std::snprintf(text, text_size, "%s", transcription.c_str());
    return ESP_OK;
}

extern "C" esp_err_t esp_transcribe_begin(void)
{
    std::lock_guard lock{mutex};
    if (pipeline_started || session)
    {
        ESP_LOGE(kTag, "esp_transcribe_begin: a listening pipeline or session is already running");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(InitLocked(), kTag, "Init failed");

    auto s = std::make_unique<Session>();
    for (int16_t *&buffer : s->buffers)
    {
        buffer = static_cast<int16_t *>(heap_caps_malloc(kChunkSize * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    }
    s->queue = xQueueCreate(s->buffers.size() + 1, sizeof(int));
    s->free_buffers = xSemaphoreCreateCounting(s->buffers.size(), s->buffers.size());
    s->done = xSemaphoreCreateBinary();
    if (s->buffers[0] == nullptr || s->buffers[1] == nullptr || s->queue == nullptr ||
        s->free_buffers == nullptr || s->done == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    s->pre_enc_chunks.reserve(kMaxChunks);
    // Allocated first and kept until the session ends, same heap layout as esp_transcribe_run()
    s->chunk = tlib::Tensor<int16_t>({kChunkSize}, false, 0);
    xSemaphoreTake(s->free_buffers, 0); // buffers[0] is being filled

    if (xTaskCreatePinnedToCore(SessionTask, "transcribe_pre", kInferenceTaskStackSize, s.get(),
                                CONFIG_ESP_TRANSCRIBE_SESSION_PRIORITY, nullptr, kInferenceTaskCore) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    session = std::move(s);
    return ESP_OK;
}

extern "C" esp_err_t esp_transcribe_push(const int16_t *pcm, size_t n_samples)
{
    // Called from the task that called esp_transcribe_begin(), no locking needed
    if (!session)
    {
        return ESP_ERR_INVALID_STATE;
    }
    Session &s{*session};
    if (pcm == nullptr || s.total + n_samples > ESP_TRANSCRIBE_MAX_SAMPLES)
    {
        return pcm == nullptr ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_SIZE;
    }
    while (n_samples > 0)
    {
        const size_t n = std::min<size_t>(n_samples, kChunkSize - s.fill);
        std::memcpy(s.buffers[s.current] + s.fill, pcm, n * sizeof(int16_t));
        s.fill += n;
        s.total += n;
        pcm += n;
        n_samples -= n;
        if (s.fill == kChunkSize)
        {
            const int index{s.current};
            xQueueSend(s.queue, &index, portMAX_DELAY);
            xSemaphoreTake(s.free_buffers, portMAX_DELAY); // Only waits if the worker falls behind
            s.current ^= 1;
            s.fill = 0;
        }
    }
    return ESP_OK;
}

extern "C" esp_err_t esp_transcribe_finish(char *text, size_t text_size)
{
    if (text == nullptr || text_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard lock{mutex};
    if (!session)
    {
        return ESP_ERR_INVALID_STATE;
    }
    std::unique_ptr<Session> s{std::move(session)};
    if (s->total == 0)
    {
        StopSessionTask(*s);
        return ESP_ERR_INVALID_SIZE;
    }

    // The last, partial chunk is zero-padded
    if (s->fill > 0)
    {
        std::memset(s->buffers[s->current] + s->fill, 0, (kChunkSize - s->fill) * sizeof(int16_t));
        const int index{s->current};
        xQueueSend(s->queue, &index, portMAX_DELAY);
    }
    StopSessionTask(*s);

    std::string transcription;
    ESP_RETURN_ON_ERROR(RunOnInferenceCore([&] { transcription = TranscribeChunks(s->pre_enc_chunks); }),
                        kTag, "Could not start the inference task");
    std::snprintf(text, text_size, "%s", transcription.c_str());
    return ESP_OK;
}

extern "C" void esp_transcribe_cancel(void)
{
    std::lock_guard lock{mutex};
    if (session)
    {
        StopSessionTask(*session);
        session.reset();
    }
}
