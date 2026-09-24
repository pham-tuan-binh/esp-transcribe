# esp-transcribe: on-device speech-to-text for the ESP32-S3.
file(GLOB ESP_TRANSCRIBE_SRCS
    src/*.cpp
    src/runtime/*.cpp
    src/tlib/src/*.cpp
    src/tlib_ops/src/*.cpp
    src/tlib_ops/src_esp32s3/*.cpp
    src/tlib_ops/src_esp32s3/*.s
    src/conformer/src/*.cpp
    src/conformer/src_esp32s3/*.cpp)

idf_component_register(
    SRCS ${ESP_TRANSCRIBE_SRCS}
    INCLUDE_DIRS include
    REQUIRES
        espressif__esp_codec_dev
    PRIV_INCLUDE_DIRS
        src/config
        src/runtime
        src/tlib/inc
        src/tlib_ops/inc
        src/tlib_ops/inc_esp32s3
        src/conformer/inc
    PRIV_REQUIRES
        esp_driver_i2s
        esp_driver_gpio
        esp_partition
        esp_psram
        spi_flash
        heap
        log
        espressif__esp-dsp)

# The model is compute bound: always optimize this component for speed,
# regardless of the application's optimization level.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

# Create an `idf.py <partition>-flash` target that writes the model into the
# model partition, and (by default) also flash the model as part of `idf.py flash`.
if(CONFIG_ESP_TRANSCRIBE_MODEL_PATH)
    idf_build_get_property(project_dir PROJECT_DIR)
    get_filename_component(ESP_TRANSCRIBE_MODEL_BIN "${CONFIG_ESP_TRANSCRIBE_MODEL_PATH}" ABSOLUTE
                           BASE_DIR "${project_dir}")
else()
    set(ESP_TRANSCRIBE_MODEL_BIN "${COMPONENT_DIR}/model/model.bin")
endif()
set(ESP_TRANSCRIBE_PARTITION "${CONFIG_ESP_TRANSCRIBE_MODEL_PARTITION}")
if(CONFIG_ESP_TRANSCRIBE_FLASH_MODEL)
    set(flash_in_project FLASH_IN_PROJECT)
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    partition_table_get_partition_info(model_offset "--partition-name ${ESP_TRANSCRIBE_PARTITION}" "offset")
    partition_table_get_partition_info(model_size "--partition-name ${ESP_TRANSCRIBE_PARTITION}" "size")
    if(NOT EXISTS "${ESP_TRANSCRIBE_MODEL_BIN}")
        message(FATAL_ERROR "esp-transcribe: model file '${ESP_TRANSCRIBE_MODEL_BIN}' does not exist.")
    endif()
    file(SIZE "${ESP_TRANSCRIBE_MODEL_BIN}" model_bin_size)
    if(NOT model_offset)
        message(WARNING "esp-transcribe: partition '${ESP_TRANSCRIBE_PARTITION}' not found in the partition "
                        "table, the model will not be flashed. See the esp-transcribe README.")
    else()
        math(EXPR model_size "${model_size}")
        if(model_size LESS model_bin_size)
            message(FATAL_ERROR "esp-transcribe: partition '${ESP_TRANSCRIBE_PARTITION}' is ${model_size} bytes, "
                                "but the model needs ${model_bin_size} bytes.")
        endif()
        esp_partition_register_target(${ESP_TRANSCRIBE_PARTITION} "${ESP_TRANSCRIBE_MODEL_BIN}"
                                      ALWAYS_PLAINTEXT ${flash_in_project})
    endif()
endif()
