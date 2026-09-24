# esp-transcribe

On-device English speech-to-text for the **ESP32-S3**, packaged as an ESP-IDF component.

It runs a distilled, int8-quantized 13M parameter conformer-CTC model (based on
[nvidia/stt_en_conformer_ctc_small](https://huggingface.co/nvidia/stt_en_conformer_ctc_small))
fully on the chip. No network, no cloud: audio never leaves the device. The model, kernels and
performance work come from [conformer-stt-s3](https://github.com/lspr98/conformer-stt-s3) by lspr98.
This repository wraps that work in a library API.

```c
#include "esp_transcribe.h"

static void on_text(const char *text, void *ctx) { printf("Heard: %s\n", text); }

void app_main(void)
{
    esp_transcribe_config_t cfg = ESP_TRANSCRIBE_DEFAULT_CONFIG();
    cfg.on_text = on_text;
    ESP_ERROR_CHECK(esp_transcribe_start_mic(&cfg));
}
```

| | |
| --- | --- |
| Accuracy | 5.6% WER on LibriSpeech clean, 11.1% on LibriSpeech other ([full table](https://github.com/lspr98/conformer-stt-s3#word-error-rate-wer)) |
| Speed | ~1.3s for 1s of audio, ~13s for 8s (dual core, 240MHz, octal PSRAM/flash) |
| Max utterance | 8.3s (23 chunks of 360ms) |
| Output | lower-case English text |

## Requirements

- ESP32-S3 with **≥ 4MB PSRAM** and **≥ 16MB flash**
- **ESP-IDF v6.1** or newer
- A microphone: an I2S MEMS mic such as the INMP441, any board with an `esp_codec_dev` record device, or your own audio source
- Memory used: 256KB of internal SRAM (static), 4MB of PSRAM (allocated at init), a 14MB flash partition for the model

Tested boards: Seeed Studio XIAO ESP32-S3 Plus (8MB octal PSRAM, 16MB flash) and ESP32-S3-DevKitC-1-N32R16V.

## Install

Pick one of the options below. Each one pulls in the model (`model/model.bin`, 13MB) and flashes it
with `idf.py flash`.

### Option A: component manager, from git (recommended)

```sh
idf.py add-dependency "esp-transcribe" --git https://github.com/pham-tuan-binh/esp-transcribe.git
```

or add it to `main/idf_component.yml` yourself:

```yaml
dependencies:
  esp-transcribe:
    git: https://github.com/pham-tuan-binh/esp-transcribe.git
    version: v1.0.0        # optional: pin a tag, branch or commit
```

### Option B: git submodule

```sh
git submodule add https://github.com/pham-tuan-binh/esp-transcribe.git components/esp-transcribe
```

### Option C: copy it in

Clone or download this repository into `components/esp-transcribe` in your project.

### Option D: local path (to develop the library itself)

```yaml
dependencies:
  esp-transcribe:
    path: ../path/to/esp-transcribe   # the directory must be named esp-transcribe
```

Then add `esp-transcribe` to your component's requirements:

```cmake
idf_component_register(SRCS main.c PRIV_REQUIRES esp-transcribe)
```

## Set up your project

esp-transcribe needs PSRAM, 16MB flash and a partition for the model. Copy
[`sdkconfig.defaults.esp-transcribe`](sdkconfig.defaults.esp-transcribe) into your
project's `sdkconfig.defaults` (then delete `sdkconfig` and rebuild), or set these options in
`idf.py menuconfig`:

```ini
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="managed_components/esp-transcribe/partitions/16MB.csv"
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y           # or CONFIG_SPIRAM_MODE_QUAD=y for quad PSRAM boards
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_ESP32S3_DATA_CACHE_32KB=y
```

### Partition table

Ready-made tables ship in [`partitions/`](partitions):

| File | Flash | Layout |
| --- | --- | --- |
| `16MB.csv` | 16MB | nvs, 1MB app, 14MB model |
| `32MB_ota.csv` | 32MB | nvs, two 4MB OTA app slots, 14MB model |

Point `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` at one of them. For a submodule or copy, use
`components/esp-transcribe/partitions/...`. To use your own table, add a data partition of at least
14MB named `model`:

```csv
# Name,   Type, SubType,   Offset,  Size,  Flags
factory,  app,  factory,   0x10000, 1M,
model,    data, undefined, ,        14M,
```

### Flash

```sh
idf.py build flash monitor
```

`idf.py flash` writes the app **and** the model. The model is 14MB, so that step is slow. Once the
model is on the device, disable **esp-transcribe → Model → Flash the model with `idf.py flash`** in
menuconfig (`CONFIG_ESP_TRANSCRIBE_FLASH_MODEL=n`). After that, `idf.py flash` writes only your app
and `idf.py model-flash` writes only the model.

## Usage

Everything is in [`include/esp_transcribe.h`](include/esp_transcribe.h) (C),
[`include/esp_transcribe_codec_dev.h`](include/esp_transcribe_codec_dev.h) (`esp_codec_dev` devices) and
[`include/esp_transcribe.hpp`](include/esp_transcribe.hpp) (C++ wrapper). All audio is 16kHz mono
`int16_t`.

### 1. Always listening on an I2S microphone

Wire the INMP441 as SCK→GPIO2, WS→GPIO3, SD→GPIO1, L/R→GND (or change the pins), then:

```c
esp_transcribe_config_t cfg = ESP_TRANSCRIBE_DEFAULT_CONFIG();
cfg.mic.bclk_gpio = 2;
cfg.mic.ws_gpio = 3;
cfg.mic.din_gpio = 1;
cfg.on_text = on_text;       // required
cfg.on_state = on_state;     // optional: IDLE / LISTENING / TRANSCRIBING, e.g. for an LED
cfg.user_ctx = my_ctx;       // passed to both callbacks
ESP_ERROR_CHECK(esp_transcribe_start_mic(&cfg));
```

The pipeline detects sound, records until the speaker pauses, transcribes, and calls `on_text`.
Callbacks run on the inference task, so keep them short.

> The INMP441 needs a clean supply. Without enough decoupling (the original author needed ~800nF on
> the XIAO), inference load shows up as noise in the recording and accuracy drops a lot. See
> [the original notes](https://github.com/lspr98/conformer-stt-s3#hardware-instructions).

### 2. Always listening on an `esp_codec_dev` device (like LiveKit)

If your board layer gives you an `esp_codec_dev_handle_t` record device, pass it in the same way you
would pass it to the LiveKit ESP32 SDK. The handle can come from `tempotian/codec_board`
(`get_record_handle()`), from a BSP, or from your own `esp_codec_dev_new()` setup (ES7210, ES8311,
a PDM mic, ...):

```c
#include "esp_transcribe_codec_dev.h"

esp_codec_dev_handle_t record_handle = get_record_handle();   // same handle LiveKit takes
esp_codec_dev_set_in_gain(record_handle, 30.0);

esp_transcribe_config_t cfg = ESP_TRANSCRIBE_DEFAULT_CONFIG();
cfg.on_text = on_text;

esp_transcribe_codec_dev_config_t dev = ESP_TRANSCRIBE_CODEC_DEV_DEFAULT_CONFIG(record_handle);
dev.channels = 4;   // e.g. ES7210 in TDM mode delivers 4 interleaved channels...
dev.channel = 0;    // ...transcribe the first one
ESP_ERROR_CHECK(esp_transcribe_start_codec_dev(&dev, &cfg));
```

esp-transcribe opens the device at 16kHz/16-bit (set `dev.skip_open = true` if you already opened it
in that format) and reads it from its own task. Only one reader can use a codec device at a time. To
run LiveKit and transcription on the same microphone, capture the audio once and pass a copy to
`esp_transcribe_feed()` (option 3).

### 3. Always listening on your own audio

Use this for a different mic driver, a codec, or a network stream. It runs the same voice activity
detection and callbacks as the microphone pipeline. Feed audio in real time, from one task:

```c
ESP_ERROR_CHECK(esp_transcribe_start_stream(&cfg));   // cfg.mic is ignored
while (true) {
    int16_t frame[320];
    read_my_audio(frame, 320);
    esp_transcribe_feed(frame, 320, portMAX_DELAY);
}
```

### 4. One-shot transcription of a buffer

```c
char text[256];
ESP_ERROR_CHECK(esp_transcribe_run(pcm, n_samples, text, sizeof(text)));   // up to 8.3s of audio
```

This blocks until done. It can't be used while a listening pipeline is running.

### C++

```cpp
#include "esp_transcribe.hpp"

esp_transcribe::StartMic([](std::string_view text) { /* ... */ });
// or: esp_transcribe::StartCodecDev(ESP_TRANSCRIBE_CODEC_DEV_DEFAULT_CONFIG(get_record_handle()), on_text);

std::string text;
esp_transcribe::Transcribe(pcm, n_samples, text);
```

### Loading the model early

The first call to any function loads the model. To catch a missing or empty partition at boot, call
`esp_transcribe_init()` first. It returns `ESP_ERR_NOT_FOUND` if there is no `model` partition,
`ESP_ERR_INVALID_STATE` if the partition doesn't hold a valid model (not flashed yet), and
`ESP_ERR_NO_MEM` if PSRAM is missing.

## Configuration

You can configure esp-transcribe two ways:

- **menuconfig**: `idf.py menuconfig` → *Component config → esp-transcribe*, or put `CONFIG_ESP_TRANSCRIBE_*`
  lines in `sdkconfig.defaults`. These set the values that `ESP_TRANSCRIBE_DEFAULT_CONFIG()` returns.
- **code**: override any field of `esp_transcribe_config_t` before starting.

| Option | Default | |
| --- | --- | --- |
| `ESP_TRANSCRIBE_MODEL_PARTITION` | `model` | Partition label for the model |
| `ESP_TRANSCRIBE_MODEL_PATH` | *(bundled)* | Flash a different model file (path relative to the project) |
| `ESP_TRANSCRIBE_FLASH_MODEL` | `y` | Include the model in `idf.py flash` |
| `ESP_TRANSCRIBE_I2S_PORT` / `_BCLK_GPIO` / `_WS_GPIO` / `_DIN_GPIO` | `0` / `2` / `3` / `1` | Default microphone wiring (`cfg.mic`) |
| `ESP_TRANSCRIBE_GAIN_SHIFT` | `11` | Mic gain: lower is louder, each step doubles it (`cfg.mic.gain_shift`) |
| `ESP_TRANSCRIBE_VAD_THRESHOLD` | `1000` | Loudness that counts as sound; raise it in noisy rooms (`cfg.vad.threshold`) |
| `ESP_TRANSCRIBE_MIN_CHUNKS` | `3` | Minimum 360ms chunks with sound to transcribe (`cfg.vad.min_chunks`) |
| `ESP_TRANSCRIBE_MAX_CHUNKS` | `23` | Transcribe once an utterance is this long; lower means faster replies (`cfg.vad.max_chunks`) |
| `ESP_TRANSCRIBE_EOS_CHUNKS` | `3` | Silent chunks that end an utterance (`cfg.vad.eos_chunks`) |
| `ESP_TRANSCRIBE_PRUNE_CHUNKS` | `2` | Trailing silent chunks dropped before transcribing (`cfg.vad.prune_chunks`) |
| `ESP_TRANSCRIBE_DUAL_CORE` | `y` | Split inference across both cores (~2x faster). Turn off to keep the other core free |
| `ESP_TRANSCRIBE_INFERENCE_CORE` | `1` | Core that runs the model; recording and the helper use the other one |
| `ESP_TRANSCRIBE_INFERENCE_PRIORITY` | `24` | Inference task priority; lower it if WiFi (23) must not be preempted |
| `ESP_TRANSCRIBE_RECORDING_PRIORITY` | `23` | Recording task priority (must be below inference) |
| `ESP_TRANSCRIBE_TASK_STACK_SIZE` | `6144` | Stack per task; raise it if your callbacks need more |

### Using it next to WiFi/BLE

The library takes 256KB of internal SRAM, which leaves little for WiFi. By default it also keeps both
cores busy at top priority while it transcribes. If you also run WiFi or BLE:

- set `ESP_TRANSCRIBE_DUAL_CORE=n` and keep your networking on the other core (transcription takes about twice as long)
- lower `ESP_TRANSCRIBE_INFERENCE_PRIORITY` below the WiFi task (e.g. 20) and `ESP_TRANSCRIBE_RECORDING_PRIORITY` below that
- move WiFi buffers to PSRAM (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`)

## Examples

- [`examples/basic`](examples/basic): microphone pipeline with a status LED (C)
- [`examples/codec_dev`](examples/codec_dev): microphone from `tempotian/codec_board` as an `esp_codec_dev` handle, same setup as the LiveKit examples (pick your board in menuconfig)
- [`examples/stream`](examples/stream): feeding your own audio, using the C++ wrapper
- [`examples/sensecap_watcher`](examples/sensecap_watcher): push-to-talk on the [SenseCAP Watcher](https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher): hold the knob, speak, let go, and the text shows up on the round display (`esp_transcribe_run()` + LVGL)

```sh
cd examples/basic
idf.py set-target esp32s3 build flash monitor
```

## How it works

The model runs out of memory-mapped flash through hand-written ESP32-S3 SIMD kernels with
power-of-two int8 quantization, split across both cores. Preprocessing runs while audio is still
being recorded. See the
[original project's write-up](https://github.com/lspr98/conformer-stt-s3#performance-optimizations)
for the details.

## License

Code: Apache-2.0 (see [LICENSE](LICENSE)). Model: derived from nvidia/stt_en_conformer_ctc_small, CC-BY-4.0
(see [model/](model)).
