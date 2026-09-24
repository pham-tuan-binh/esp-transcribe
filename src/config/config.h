#ifndef CONFIG_H_
#define CONFIG_H_

#include <cstdint>
#include <cstddef>

#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>

/**
 * Internal compile-time configuration of esp-transcribe. Values that users are
 * expected to change (pins, gain, voice activity thresholds) are runtime
 * parameters, see esp_transcribe.h. The defaults below are used for those.
 */

/**
 * Audio
 */
// Sampling rate in Hz. The model is trained on 16kHz audio only.
inline constexpr const uint32_t kSamplingRate{16'000U};

/**
 * Voice activity detection
 */
// Maximum amount of total chunks allowed for a single forward pass. This is a hard
// limit: longer inputs do not fit into the SRAM heap.
inline constexpr const uint8_t kMaxChunks{23};

/**
 * Recording/Preprocessing
 */
// Number of audio samples per chunk
inline constexpr const uint32_t kChunkSize{160 * 36};
// Size of the fourier transform. kNFFT/2 must be a power of 4 due to usage of the radix-4 FFT
inline constexpr const uint16_t kNFFT{512};
// Window length for the input of the fourier transform
inline constexpr const uint16_t kWinLength{512};
// Hop length for the short-time fourier transform
inline constexpr const uint16_t kHopLength{160};

/**
 * RAM
 */
// Max numbers of blocks per heap
inline constexpr const uint8_t kHeapMaxBlocks{64};
// Byte alignment of heap
inline constexpr const uint8_t kHeapAlignment{16};
// Size of heap in SRAM for dynamically allocated tensors
inline constexpr const size_t kSRAMHeapSize{256 * 1'024};
// Size of heap in PSRAM for dynamically allocated tensors
inline constexpr const size_t kPSRAMHeapSize{4 * 1'024 * 1'024};

/**
 * Flash
 */
// Max numbers of tensor dimensions in model file
inline constexpr const uint8_t kFlashTensorMaxDims{4};
// Tensor data byte alignment in model file
inline constexpr const uint8_t kFlashTensorAlignment{16};
// Size of meta data partition in model file (in bytes)
inline constexpr const uint32_t kFlashMetaPartitionSize{64*1'024};

/**
 * Task (see Kconfig)
 */
#if CONFIG_FREERTOS_UNICORE
#error "esp-transcribe needs both cores: disable CONFIG_FREERTOS_UNICORE"
#endif

// Stack size for inference task
inline constexpr const uint32_t kInferenceTaskStackSize{CONFIG_ESP_TRANSCRIBE_TASK_STACK_SIZE};
// Stack size for recording task
inline constexpr const uint32_t kRecordingTaskStackSize{CONFIG_ESP_TRANSCRIBE_TASK_STACK_SIZE};
// Stack size for co inference task
inline constexpr const uint32_t kCoInferenceTaskStackSize{CONFIG_ESP_TRANSCRIBE_TASK_STACK_SIZE};

// Whether the co inference task is used to split matrix multiplications across both cores
#ifdef CONFIG_ESP_TRANSCRIBE_DUAL_CORE
inline constexpr const bool kDualCore{true};
#else
inline constexpr const bool kDualCore{false};
#endif

// Core on which the inference task should run
inline constexpr const uint8_t kInferenceTaskCore{CONFIG_ESP_TRANSCRIBE_INFERENCE_CORE};
// Core on which the recording task should run
inline constexpr const uint8_t kRecordingTaskCore{1 - kInferenceTaskCore};
// Core on which the co inference task should run. Needs to be different than the inference task core.
inline constexpr const uint8_t kCoInferenceTaskCore{1 - kInferenceTaskCore};
static_assert(kInferenceTaskCore != kRecordingTaskCore);
static_assert(kInferenceTaskCore != kCoInferenceTaskCore);

// Task priority of the inference task
inline constexpr const uint8_t kInferenceTaskPriority{CONFIG_ESP_TRANSCRIBE_INFERENCE_PRIORITY};
// Task priority of the recording task
inline constexpr const uint8_t kRecordingTaskPriority{CONFIG_ESP_TRANSCRIBE_RECORDING_PRIORITY};
// Task priority of the co inference task. Needs to be higher than the recording task priority
inline constexpr const uint8_t kCoInferenceTaskPriority{kInferenceTaskPriority};
static_assert(kCoInferenceTaskPriority > kRecordingTaskPriority,
              "CONFIG_ESP_TRANSCRIBE_RECORDING_PRIORITY must be lower than CONFIG_ESP_TRANSCRIBE_INFERENCE_PRIORITY");
static_assert(kInferenceTaskPriority < configMAX_PRIORITIES);

#endif
