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
    src/common/nes.c
    src/common/nrom.c
    src/common/ppu.c
)

if (NOT MICRONES_PLATFORM STREQUAL "pico")
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
    set(MICRONES_PICO_COMMON_SOURCES
        src/pico/main.c
        src/pico/nes_hw_controller.c
        src/pico/pico_input.c
        src/pico/pico_time.c
    )
    set(MICRONES_PICO_COMMON_LIBS
        hardware_clocks
        hardware_gpio
        hardware_irq
        hardware_pwm
        hardware_timer
        hardware_vreg
        m
        pico_stdlib
    )
    set(MICRONES_PICO_ROM_SOURCES)

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
        list(APPEND MICRONES_PICO_ROM_SOURCES
            "${MICRONES_PICO_ROM_C}"
        )
    endif ()

    function(micrones_add_pico_core target_name framebuffer_enabled)
        add_library(${target_name} STATIC
            ${MICRONES_COMMON_SOURCES}
        )
        target_include_directories(${target_name} PUBLIC
            ${CMAKE_CURRENT_LIST_DIR}/src/common
        )
        target_compile_definitions(${target_name} PUBLIC
            MICRONES_APU_ENGINE_MICRONES=${MICRONES_APU_ENGINE_MICRONES_FLAG}
            MICRONES_ENABLE_RUNTIME_DIAGNOSTICS=0
            MICRONES_ENABLE_APU_DEBUG_METRICS=0
            MICRONES_ENABLE_APU_PCM_OUTPUT=1
            MICRONES_ENABLE_APU_EMULATION=1
            MICRONES_ENABLE_FRAMEBUFFER=${framebuffer_enabled}
            MICRONES_ENABLE_STEP_PROFILING=0
            MICRONES_PICO_PLATFORM=1
        )
        target_link_libraries(${target_name} PUBLIC pico_base_headers)
        if (NOT MICRONES_APU_ENGINE STREQUAL "infones")
            target_link_libraries(${target_name} PUBLIC m)
        endif ()
    endfunction()

    function(micrones_add_pico_target target_name backend_name core_target audio_backend_name)
        add_executable(${target_name}
            ${MICRONES_PICO_COMMON_SOURCES}
            ${MICRONES_PICO_ROM_SOURCES}
        )

        target_include_directories(${target_name} PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/src/pico
        )

        target_link_libraries(${target_name} PRIVATE
            ${MICRONES_PICO_COMMON_LIBS}
            ${core_target}
        )

        if (MICRONES_PICO_VIDEO_MODE STREQUAL "emulator")
            target_compile_definitions(${target_name} PRIVATE MICRONES_PICO_VIDEO_MODE_EMULATOR=1)
        else ()
            target_compile_definitions(${target_name} PRIVATE MICRONES_PICO_VIDEO_MODE_TEST_PATTERN=1)
        endif ()

        target_compile_definitions(${target_name} PRIVATE MICRONES_ENABLE_PICO_VIDEO_STATS=0)
        pico_enable_stdio_usb(${target_name} 1)
        pico_enable_stdio_uart(${target_name} 0)

        if (audio_backend_name STREQUAL "pwm")
            target_sources(${target_name} PRIVATE
                src/pico/audio_pwm.c
                src/pico/pico_audio_backend_pwm.c
            )
        elseif (audio_backend_name STREQUAL "max98357")
            target_sources(${target_name} PRIVATE
                src/pico/audio_i2s_max98357.c
                src/pico/pico_audio_backend_max98357.c
            )
            target_link_libraries(${target_name} PRIVATE
                hardware_dma
                hardware_pio
            )
            pico_generate_pio_header(${target_name}
                ${CMAKE_CURRENT_LIST_DIR}/src/pico/audio_i2s_max98357.pio
            )
        else ()
            message(FATAL_ERROR "Unknown Pico audio backend '${audio_backend_name}'")
        endif ()

        if (backend_name STREQUAL "analog")
            target_sources(${target_name} PRIVATE
                src/pico/pico_video_backend_analog.c
                src/pico/video_ntsc.c
            )
            target_link_libraries(${target_name} PRIVATE
                hardware_dma
                hardware_pio
                pico_multicore
            )
            target_compile_definitions(${target_name} PRIVATE MICRONES_PICO_VIDEO_BACKEND_ANALOG=1)

            if (MICRONES_PICO_VIDEO_MODE STREQUAL "emulator")
                target_sources(${target_name} PRIVATE
                    src/pico/core1_video.c
                    src/pico/emulator_video_adapter.c
                    src/pico/scanline_queue.c
                )
            endif ()

            pico_generate_pio_header(${target_name}
                ${CMAKE_CURRENT_LIST_DIR}/src/pico/video_ntsc.pio
            )
        elseif (backend_name STREQUAL "tft")
            target_sources(${target_name} PRIVATE
                src/pico/pico_video_backend_tft.c
                src/pico/video_tft_ili9341.c
            )
            target_link_libraries(${target_name} PRIVATE
                hardware_spi
            )
            target_compile_definitions(${target_name} PRIVATE MICRONES_PICO_VIDEO_BACKEND_TFT=1)
            # 250 MHz gives CPSDVSR=4, SCR=0 → exactly 62.5 MHz SPI (63.6 FPS ceiling).
            # 315 MHz gives best ≤62.5 MHz = 52.5 MHz (53.4 FPS ceiling) — wrong for TFT.
            # This overrides the #ifndef default in clock_config.h for this target only;
            # the analog target continues to use 315 MHz via the default.
            target_compile_definitions(${target_name} PRIVATE MICRONES_SYS_CLK_MHZ=250)

            if (MICRONES_PICO_VIDEO_MODE STREQUAL "emulator")
                target_sources(${target_name} PRIVATE
                    src/pico/emulator_video_adapter_tft.c
                )
            endif ()
        else ()
            message(FATAL_ERROR "Unknown Pico backend '${backend_name}'")
        endif ()

        pico_add_extra_outputs(${target_name})
    endfunction()

    micrones_add_pico_core(micrones_core_pico_analog 0)
    micrones_add_pico_core(micrones_core_pico_tft 1)
    micrones_add_pico_target(micrones_pico_analog analog micrones_core_pico_analog pwm)
    micrones_add_pico_target(micrones_pico_tft tft micrones_core_pico_tft pwm)
    micrones_add_pico_target(micrones_pico_tft_max98357 tft micrones_core_pico_tft max98357)
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
