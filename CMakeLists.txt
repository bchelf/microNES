cmake_minimum_required(VERSION 3.16)

set(MICRONES_PLATFORM "pico" CACHE STRING "Build platform: host, pico, or web")
set_property(CACHE MICRONES_PLATFORM PROPERTY STRINGS host pico web)
set(MICRONES_PICO_VIDEO_MODE "test_pattern" CACHE STRING "Pico video mode: test_pattern or emulator")
set_property(CACHE MICRONES_PICO_VIDEO_MODE PROPERTY STRINGS test_pattern emulator)
set(MICRONES_PICO_ROM_PATH "" CACHE FILEPATH "Path to an iNES ROM image to embed for Pico emulator video mode")
set(MICRONES_APU_ENGINE "micrones" CACHE STRING "APU engine: micrones (cycle-accurate, default) or infones (InfoNES DDS)")
set_property(CACHE MICRONES_APU_ENGINE PROPERTY STRINGS micrones infones)

if (MICRONES_PLATFORM STREQUAL "pico")
    set(PICO_BOARD pico2 CACHE STRING "Target board")
    include(pico_sdk_import.cmake)
endif ()

project(micrones C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (MICRONES_APU_ENGINE STREQUAL "infones")
    set(MICRONES_APU_SOURCE src/common/apu_infones.c)
    set(MICRONES_APU_ENGINE_MICRONES_FLAG 0)
else ()
    set(MICRONES_APU_SOURCE src/common/apu_micrones.c)
    set(MICRONES_APU_ENGINE_MICRONES_FLAG 1)
endif ()

set(MICRONES_COMMON_SOURCES
    ${MICRONES_APU_SOURCE}
    src/common/cart.c
    src/common/cpu6502.c
    src/common/cpu6502_opcode.c
    src/common/frame_pacer.c
    src/common/input.c
    src/common/mmc1.c
    src/common/mmc3.c
    src/common/nes.c
    src/common/nrom.c
    src/common/ppu.c
)

add_library(micrones_core STATIC
    ${MICRONES_COMMON_SOURCES}
)

target_include_directories(micrones_core PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/src/common
)

target_compile_definitions(micrones_core PUBLIC
    MICRONES_APU_ENGINE_MICRONES=${MICRONES_APU_ENGINE_MICRONES_FLAG}
)

if (NOT MICRONES_APU_ENGINE STREQUAL "infones")
    target_link_libraries(micrones_core PUBLIC m)
endif ()

if (MICRONES_PLATFORM STREQUAL "host")
    find_package(SDL3 QUIET CONFIG)

    add_executable(micrones_smoke
        src/host/png_write.c
        src/host/smoke_main.c
        src/host/video_capture.c
    )

    target_link_libraries(micrones_smoke PRIVATE
        micrones_core
    )

    if (SDL3_FOUND)
        add_executable(micrones
            src/host/audio_sdl.c
            src/host/run_main.c
            src/host/wav_write.c
            src/host/window_sdl.c
        )

        target_link_libraries(micrones PRIVATE
            SDL3::SDL3
            micrones_core
        )
    else ()
        message(WARNING "SDL3 not found; micrones_run will not be built.")
    endif ()
elseif (MICRONES_PLATFORM STREQUAL "pico")
    pico_sdk_init()

    target_compile_definitions(micrones_core PUBLIC
        MICRONES_ENABLE_RUNTIME_DIAGNOSTICS=0
        MICRONES_ENABLE_APU_DEBUG_METRICS=0
        MICRONES_ENABLE_APU_PCM_OUTPUT=1
        MICRONES_ENABLE_APU_EMULATION=1
        MICRONES_ENABLE_FRAMEBUFFER=0
        MICRONES_ENABLE_STEP_PROFILING=0
        MICRONES_PICO_PLATFORM=1
    )

    # Give micrones_core access to pico/platform.h so MICRONES_HOT_FUNC can expand
    # to __not_in_flash_func and place hot functions in SRAM.
    target_link_libraries(micrones_core PUBLIC pico_base_headers)

    set(MICRONES_PICO_SOURCES
        src/pico/audio_pwm.c
        src/pico/main.c
        src/pico/nes_hw_controller.c
        src/pico/pico_input.c
        src/pico/pico_time.c
        src/pico/video_ntsc.c
    )

    if (MICRONES_PICO_VIDEO_MODE STREQUAL "emulator")
        if (MICRONES_PICO_ROM_PATH STREQUAL "")
            message(FATAL_ERROR "MICRONES_PICO_ROM_PATH must be set when MICRONES_PICO_VIDEO_MODE=emulator")
        endif ()

        file(READ "${MICRONES_PICO_ROM_PATH}" MICRONES_PICO_ROM_HEX HEX)
        string(LENGTH "${MICRONES_PICO_ROM_HEX}" MICRONES_PICO_ROM_HEX_LEN)
        math(EXPR MICRONES_PICO_ROM_BYTE_COUNT "${MICRONES_PICO_ROM_HEX_LEN} / 2")
        string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1, " MICRONES_PICO_ROM_BYTES "${MICRONES_PICO_ROM_HEX}")
        set(MICRONES_PICO_ROM_C "${CMAKE_CURRENT_BINARY_DIR}/micrones_pico_embedded_rom.c")
        file(WRITE "${MICRONES_PICO_ROM_C}"
"unsigned char micrones_pico_embedded_rom[] = { ${MICRONES_PICO_ROM_BYTES} };\nunsigned int micrones_pico_embedded_rom_len = ${MICRONES_PICO_ROM_BYTE_COUNT}u;\n")
        list(APPEND MICRONES_PICO_SOURCES
            src/pico/core1_video.c
            src/pico/emulator_video_adapter.c
            src/pico/scanline_queue.c
            "${MICRONES_PICO_ROM_C}"
        )
    endif ()

    add_executable(micrones
        ${MICRONES_PICO_SOURCES}
    )

    pico_generate_pio_header(micrones
        ${CMAKE_CURRENT_LIST_DIR}/src/pico/video_ntsc.pio
    )

    target_include_directories(micrones PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src/pico
    )

    target_link_libraries(micrones PRIVATE
        hardware_clocks
        hardware_dma
        hardware_gpio
        hardware_irq
        hardware_pio
        hardware_pwm
        hardware_timer
        hardware_vreg
        m
        pico_multicore
        pico_stdlib
        micrones_core
    )

    if (MICRONES_PICO_VIDEO_MODE STREQUAL "emulator")
        target_compile_definitions(micrones PRIVATE MICRONES_PICO_VIDEO_MODE_EMULATOR=1)
    else ()
        target_compile_definitions(micrones PRIVATE MICRONES_PICO_VIDEO_MODE_TEST_PATTERN=1)
    endif ()

    target_compile_definitions(micrones PRIVATE MICRONES_ENABLE_PICO_VIDEO_STATS=0)

    pico_enable_stdio_usb(micrones 1)
    pico_enable_stdio_uart(micrones 0)
    pico_add_extra_outputs(micrones)
elseif (MICRONES_PLATFORM STREQUAL "web")
    add_executable(micrones_wasm
        src/web/web_main.c
    )

    target_link_libraries(micrones_wasm PRIVATE
        micrones_core
    )

    set_target_properties(micrones_wasm PROPERTIES
        OUTPUT_NAME "micrones"
        SUFFIX ".js"
    )

    target_link_options(micrones_wasm PRIVATE
        "SHELL:-sWASM=1"
        "SHELL:-sNO_EXIT_RUNTIME=1"
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        "SHELL:-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','HEAPU8','HEAP8']"
        "SHELL:-sEXPORTED_FUNCTIONS=['_nes_web_init','_nes_web_load_rom','_nes_web_step_frame','_nes_web_framebuffer_ptr','_nes_web_audio_sample_rate','_nes_web_audio_read_samples','_nes_web_set_controller','_nes_web_last_error','_nes_web_reset','_malloc','_free']"
    )

    # Copy the HTML frontend into the build directory alongside the JS/WASM output.
    add_custom_command(TARGET micrones_wasm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_LIST_DIR}/src/web/index.html
            ${CMAKE_CURRENT_BINARY_DIR}/index.html
        COMMENT "Copying web frontend to build directory"
    )
else ()
    message(FATAL_ERROR "Unsupported MICRONES_PLATFORM='${MICRONES_PLATFORM}'. Use 'host', 'pico', or 'web'.")
endif ()
