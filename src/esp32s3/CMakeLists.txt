cmake_minimum_required(VERSION 3.16)

# ROM path can be overridden at configure time, e.g.:
#   idf.py -DMICRONES_ROM_PATH=/absolute/path/to/game.nes build
# Default is roms/smb1.nes in the repository root.
set(MICRONES_ROM_PATH
    "${CMAKE_CURRENT_LIST_DIR}/../../roms/smb1.nes"
    CACHE STRING
    "Path to the .nes ROM file to embed in the ESP32-S3 firmware")

# ESP-IDF project: must include IDF cmake before project().
# Set IDF_PATH in the environment before running cmake/idf.py.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(micrones_esp32s3)
