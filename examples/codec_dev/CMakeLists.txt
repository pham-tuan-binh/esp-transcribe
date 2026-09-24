cmake_minimum_required(VERSION 3.22)
# Recommended esp-transcribe settings (flash size, PSRAM, speed), then this project's own overrides.
set(SDKCONFIG_DEFAULTS "../../sdkconfig.defaults.esp-transcribe;sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-transcribe-codec-dev)
