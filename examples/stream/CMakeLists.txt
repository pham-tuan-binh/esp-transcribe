cmake_minimum_required(VERSION 3.22)
# Recommended esp-transcribe settings (flash size, PSRAM, speed), then this project's own overrides.
set(SDKCONFIG_DEFAULTS "../../sdkconfig.defaults.esp-transcribe;sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp-transcribe-stream)
