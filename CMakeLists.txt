#
# xeforth root CMakeLists.txt
#
cmake_minimum_required(VERSION 3.16)

# Force the target to ESP32-S3 if it hasn't been set by the environment yet
if(NOT DEFINED ENV{IDF_TARGET} AND NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32s3" CACHE STRING "Forcing ESP32-S3 architecture")
endif()

# Include the core ESP-IDF build logic
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Safety Guard: Throw a hard error if someone tries to switch away from esp32s3
if(NOT "${IDF_TARGET}" STREQUAL "esp32s3")
    message(FATAL_ERROR "This repository is hardware-specific and only supports compilation for ESP32-S3!")
endif()

project(xeforth)

# `cmake -B build`                             create ~/build directory
# `cmake --build build --target clean`      == old `make clean` (whole-project
#                                              only; CMake has no free
#                                              per-module clean-mu equivalent)
# `ninja -C build -t clean mu`              == old `make clean-mu`
