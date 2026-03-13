cmake_minimum_required(VERSION 3.16)

set(SMB2350_PLATFORM "pico" CACHE STRING "Build platform: host or pico")
set_property(CACHE SMB2350_PLATFORM PROPERTY STRINGS host pico)

if (SMB2350_PLATFORM STREQUAL "pico")
    set(PICO_BOARD pico2 CACHE STRING "Target board")
    include(pico_sdk_import.cmake)
endif ()

project(smb2350 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SMB2350_COMMON_SOURCES
    src/common/apu.c
    src/common/cart.c
    src/common/cpu6502.c
    src/common/cpu6502_opcode.c
    src/common/input.c
    src/common/nes.c
    src/common/nrom.c
    src/common/ppu.c
)

add_library(smb2350_core STATIC
    ${SMB2350_COMMON_SOURCES}
)

target_include_directories(smb2350_core PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/src/common
)

if (SMB2350_PLATFORM STREQUAL "host")
    find_package(SDL3 QUIET CONFIG)

    add_executable(smb2350_smoke
        src/host/png_write.c
        src/host/smoke_main.c
        src/host/video_capture.c
    )

    target_link_libraries(smb2350_smoke PRIVATE
        smb2350_core
    )

    if (SDL3_FOUND)
        add_executable(smb2350_run
            src/host/run_main.c
            src/host/window_sdl.c
        )

        target_link_libraries(smb2350_run PRIVATE
            SDL3::SDL3
            smb2350_core
        )
    else ()
        message(WARNING "SDL3 not found; smb2350_run will not be built.")
    endif ()
elseif (SMB2350_PLATFORM STREQUAL "pico")
    pico_sdk_init()

    add_executable(smb2350
        src/pico/audio_pwm.c
        src/pico/main.c
        src/pico/video_ntsc.c
    )

    pico_generate_pio_header(smb2350
        ${CMAKE_CURRENT_LIST_DIR}/src/pico/video_ntsc.pio
    )

    target_include_directories(smb2350 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src/pico
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
else ()
    message(FATAL_ERROR "Unsupported SMB2350_PLATFORM='${SMB2350_PLATFORM}'. Use 'host' or 'pico'.")
endif ()
