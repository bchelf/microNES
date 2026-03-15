cmake_minimum_required(VERSION 3.16)

set(SMB2350_PLATFORM "pico" CACHE STRING "Build platform: host or pico")
set_property(CACHE SMB2350_PLATFORM PROPERTY STRINGS host pico)
set(SMB2350_PICO_VIDEO_MODE "test_pattern" CACHE STRING "Pico video mode: test_pattern or emulator")
set_property(CACHE SMB2350_PICO_VIDEO_MODE PROPERTY STRINGS test_pattern emulator)
set(SMB2350_PICO_ROM_PATH "" CACHE FILEPATH "Path to an iNES ROM image to embed for Pico emulator video mode")

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
    src/common/frame_pacer.c
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
            src/host/audio_sdl.c
            src/host/run_main.c
            src/host/wav_write.c
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

    target_compile_definitions(smb2350_core PUBLIC
        SMB2350_ENABLE_RUNTIME_DIAGNOSTICS=0
        SMB2350_ENABLE_APU_DEBUG_METRICS=0
        SMB2350_ENABLE_APU_PCM_OUTPUT=0
        SMB2350_ENABLE_APU_EMULATION=0
        SMB2350_ENABLE_FRAMEBUFFER=0
        SMB2350_ENABLE_STEP_PROFILING=1
    )

    set(SMB2350_PICO_SOURCES
        src/pico/audio_pwm.c
        src/pico/main.c
        src/pico/pico_time.c
        src/pico/video_ntsc.c
    )

    if (SMB2350_PICO_VIDEO_MODE STREQUAL "emulator")
        if (SMB2350_PICO_ROM_PATH STREQUAL "")
            message(FATAL_ERROR "SMB2350_PICO_ROM_PATH must be set when SMB2350_PICO_VIDEO_MODE=emulator")
        endif ()

        file(READ "${SMB2350_PICO_ROM_PATH}" SMB2350_PICO_ROM_HEX HEX)
        string(LENGTH "${SMB2350_PICO_ROM_HEX}" SMB2350_PICO_ROM_HEX_LEN)
        math(EXPR SMB2350_PICO_ROM_BYTE_COUNT "${SMB2350_PICO_ROM_HEX_LEN} / 2")
        string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1, " SMB2350_PICO_ROM_BYTES "${SMB2350_PICO_ROM_HEX}")
        set(SMB2350_PICO_ROM_C "${CMAKE_CURRENT_BINARY_DIR}/smb2350_pico_embedded_rom.c")
        file(WRITE "${SMB2350_PICO_ROM_C}"
"unsigned char smb2350_pico_embedded_rom[] = { ${SMB2350_PICO_ROM_BYTES} };\nunsigned int smb2350_pico_embedded_rom_len = ${SMB2350_PICO_ROM_BYTE_COUNT}u;\n")
        list(APPEND SMB2350_PICO_SOURCES
            src/pico/core1_video.c
            src/pico/emulator_video_adapter.c
            src/pico/scanline_queue.c
            "${SMB2350_PICO_ROM_C}"
        )
    endif ()

    add_executable(smb2350
        ${SMB2350_PICO_SOURCES}
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
        pico_multicore
        pico_stdlib
        smb2350_core
    )

    if (SMB2350_PICO_VIDEO_MODE STREQUAL "emulator")
        target_compile_definitions(smb2350 PRIVATE SMB2350_PICO_VIDEO_MODE_EMULATOR=1)
    else ()
        target_compile_definitions(smb2350 PRIVATE SMB2350_PICO_VIDEO_MODE_TEST_PATTERN=1)
    endif ()

    target_compile_definitions(smb2350 PRIVATE SMB2350_ENABLE_PICO_VIDEO_STATS=0)

    pico_enable_stdio_usb(smb2350 1)
    pico_enable_stdio_uart(smb2350 0)
    pico_add_extra_outputs(smb2350)
else ()
    message(FATAL_ERROR "Unsupported SMB2350_PLATFORM='${SMB2350_PLATFORM}'. Use 'host' or 'pico'.")
endif ()
