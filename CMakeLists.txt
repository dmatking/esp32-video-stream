# Required CMake Version
cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS
    "sdkconfig.defaults"
    "$ENV{HOME}/.esp_creds"
)

# Set up ESP-IDF build environment
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project name
project(video-stream)

