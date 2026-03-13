cmake_minimum_required(VERSION 3.16)

set(PICO_BOARD pico2 CACHE STRING "Target board")

include(pico_sdk_import.cmake)

project(smb2350 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

pico_sdk_init()

add_executable(smb2350
    src/audio_pwm.c
    src/main.c
    src/video_ntsc.c
)

pico_generate_pio_header(smb2350
    ${CMAKE_CURRENT_LIST_DIR}/src/video_ntsc.pio
)

target_include_directories(smb2350 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_link_libraries(smb2350 PRIVATE
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_pio
    hardware_pwm
    m
    pico_stdlib
)

pico_enable_stdio_usb(smb2350 1)
pico_enable_stdio_uart(smb2350 0)
pico_add_extra_outputs(smb2350)
