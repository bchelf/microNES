cmake_minimum_required(VERSION 3.16)

set(MICRONES_PLATFORM "pico" CACHE STRING "Build platform: host, pico, or web")
set_property(CACHE MICRONES_PLATFORM PROPERTY STRINGS host pico web)
set(MICRONES_PICO_VIDEO_MODE "test_pattern" CACHE STRING "Pico video mode: test_pattern or emulator")
set_property(CACHE MICRONES_PICO_VIDEO_MODE PROPERTY STRINGS test_pattern emulator)
set(MICRONES_PICO_TFT_BACKEND "spi_ili9341" CACHE STRING "Pico TFT backend: spi_ili9341 or parallel_8080")
set_property(CACHE MICRONES_PICO_TFT_BACKEND PROPERTY STRINGS spi_ili9341 parallel_8080)
set(MICRONES_PICO_TFT_CONTROLLER "ili9341" CACHE STRING "Pico TFT controller init: ili9341, ili9486, or ili9488")
set_property(CACHE MICRONES_PICO_TFT_CONTROLLER PROPERTY STRINGS ili9341 ili9486 ili9488)
set(MICRONES_APU_ENGINE "micrones" CACHE STRING "APU engine: micrones (cycle-accurate, default) or infones (InfoNES DDS)")
set_property(CACHE MICRONES_APU_ENGINE PROPERTY STRINGS micrones infones)
# Select which physical board the Pico firmware should target.
#   breadboard  -> legacy breadboard rig (see SCHEMATIC.md, audio_pwm.h,
#                  pico_input.h, video_ntsc.h defaults).
#   v0_1        -> microNES PCB v0.1, sourced from
#                  hardware/microNES_pcb_v0.1/microNES_pcb_v0.1.kicad_sch.
#                  Drives MICRONES_BOARD_V0_1 into every pin header, which
#                  then resolves to the schematic's pin assignments.
set(MICRONES_BOARD "breadboard" CACHE STRING "Pico target board: breadboard or v0_1")
set_property(CACHE MICRONES_BOARD PROPERTY STRINGS breadboard v0_1)
option(MICRONES_ENABLE_PERF_LOG "Enable per-second performance/audio diagnostic logging" OFF)
option(MICRONES_HDMI_STDIO_USB "Enable USB CDC stdio for HDMI builds" ON)
option(MICRONES_HDMI_TEST_TONE "Generate HDMI audio packets from a built-in 440 Hz diagnostic tone" OFF)

if (MICRONES_PLATFORM STREQUAL "pico")
    set(PICO_BOARD pico2 CACHE STRING "Target board")
    set(PICO_FLASH_SIZE_BYTES "16 * 1024 * 1024" CACHE STRING "Target board flash size")
    set(MICRONES_PICO_FLASH_CACHE_OFFSET "4 * 1024 * 1024" CACHE STRING "Flash offset reserved for one cached ROM")
    set(MICRONES_PICO_FLASH_CACHE_SIZE "0" CACHE STRING "Flash ROM cache size; 0 means from offset to end of flash")
    option(MICRONES_PICO_ENABLE_FLASH_ROM_CACHE "Copy selected SD ROMs to XIP flash before launch" ON)
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
    src/common/app_shell.c
    src/common/axrom.c
    src/common/cart.c
    src/common/cnrom.c
    src/common/colordreams.c
    src/common/cpu6502.c
    src/common/cpu6502_opcode.c
    src/common/font_5x7.c
    src/common/frame_pacer.c
    src/common/gxrom.c
    src/common/input.c
    src/common/mapper40.c
    src/common/mmc1.c
    src/common/mmc2.c
    src/common/mmc3.c
    src/common/nes.c
    src/common/nrom.c
    src/common/ppu.c
    src/common/rom_menu.c
    src/common/smb1_bg_classifier.c
    src/common/uxrom.c
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

    add_executable(micrones_menu_smoke
        tests/menu_smoke.c
    )

    target_link_libraries(micrones_menu_smoke PRIVATE
        micrones_core
    )

    add_executable(hdmi_data_island_test
        src/host/hdmi_data_island_test.c
        src/pico/hdmi_data_island.c
    )
    target_include_directories(hdmi_data_island_test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src/pico
    )

    if (SDL3_FOUND)
        add_executable(micrones
            src/host/audio_sdl.c
            src/host/rom_source_posix.c
            src/host/run_main.c
            src/host/wav_write.c
            src/host/window_sdl.c
        )

        target_link_libraries(micrones PRIVATE
            SDL3::SDL3
            micrones_core
        )

        if (MICRONES_ENABLE_PERF_LOG)
            target_compile_definitions(micrones PRIVATE MICRONES_ENABLE_PERF_LOG=1)
        endif ()
    else ()
        message(WARNING "SDL3 not found; micrones_run will not be built.")
    endif ()
elseif (MICRONES_PLATFORM STREQUAL "pico")
    pico_sdk_init()
    set(MICRONES_PICO_COMMON_SOURCES
        src/pico/fat32.c
        src/pico/main.c
        src/pico/nes_hw_controller.c
        src/pico/pico_input.c
        src/pico/pico_status.c
        src/pico/pico_time.c
        src/pico/rom_source_flash_cache.c
        src/pico/rom_source_flash_fs.c
        src/pico/rom_source_sd.c
        src/pico/sd_spi.c
    )
    set(MICRONES_PICO_COMMON_LIBS
        hardware_clocks
        hardware_gpio
        hardware_irq
        hardware_pio
        hardware_pwm
        hardware_timer
        hardware_vreg
        hardware_flash
        m
        pico_multicore
        pico_stdlib
    )
    # ROMs now load from the SD card at runtime; no embedded image, no
    # MICRONES_PICO_ROM_PATH needed.
    set(MICRONES_PICO_ROM_SOURCES)

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
            MICRONES_CHR_ROW_CACHE_MAX_BYTES=32768
            MICRONES_PICO_PRG_SRAM_COPY_MAX=65536
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

        # Propagate the selected physical board into the pin headers, which
        # then resolve to the matching schematic pin assignments.
        if (MICRONES_BOARD STREQUAL "v0_1")
            target_compile_definitions(${target_name} PRIVATE MICRONES_BOARD_V0_1=1)
        endif ()

        target_compile_definitions(${target_name} PRIVATE MICRONES_ENABLE_PICO_VIDEO_STATS=0)
        target_compile_definitions(${target_name} PRIVATE PICO_MALLOC_PANIC=0)

        if (MICRONES_PICO_ENABLE_FLASH_ROM_CACHE)
            if (MICRONES_PICO_FLASH_CACHE_SIZE STREQUAL "0")
                math(EXPR _micrones_flash_cache_size "${PICO_FLASH_SIZE_BYTES} - (${MICRONES_PICO_FLASH_CACHE_OFFSET})")
            else ()
                math(EXPR _micrones_flash_cache_size "${MICRONES_PICO_FLASH_CACHE_SIZE}")
            endif ()
            if (_micrones_flash_cache_size LESS 0)
                message(FATAL_ERROR "MICRONES_PICO_FLASH_CACHE_OFFSET is beyond PICO_FLASH_SIZE_BYTES")
            endif ()
            target_compile_definitions(${target_name} PRIVATE
                MICRONES_PICO_ENABLE_FLASH_ROM_CACHE=1
                MICRONES_PICO_FLASH_CACHE_OFFSET=${MICRONES_PICO_FLASH_CACHE_OFFSET}
                MICRONES_PICO_FLASH_CACHE_SIZE=${_micrones_flash_cache_size}
            )
        else ()
            target_compile_definitions(${target_name} PRIVATE
                MICRONES_PICO_ENABLE_FLASH_ROM_CACHE=0
                MICRONES_PICO_FLASH_CACHE_OFFSET=0
                MICRONES_PICO_FLASH_CACHE_SIZE=0
            )
        endif ()

        if (MICRONES_ENABLE_PERF_LOG)
            target_compile_definitions(${target_name} PRIVATE MICRONES_ENABLE_PERF_LOG=1)
        endif ()
        pico_enable_stdio_usb(${target_name} 1)
        pico_enable_stdio_uart(${target_name} 0)

        # SD card driver runs SPI through PIO so the v0.1 board's pin map works.
        pico_generate_pio_header(${target_name}
            ${CMAKE_CURRENT_LIST_DIR}/src/pico/sd_spi.pio
        )

        # Forward any -DMICRONES_SD_* override the user passed at configure
        # time into the compile.  sd_card_pins.h #ifndefs the names so a
        # symbol defined here wins over the default; symbols not passed are
        # simply absent and the header default applies.
        foreach (_sd_var
            MICRONES_SD_PIN_CD MICRONES_SD_PIN_MISO MICRONES_SD_PIN_SCK
            MICRONES_SD_PIN_MOSI MICRONES_SD_PIN_CS
            MICRONES_SD_PIO_INDEX
            MICRONES_SD_INIT_BAUD_HZ MICRONES_SD_RUN_BAUD_HZ
            MICRONES_SD_CD_ACTIVE_LOW MICRONES_SD_CD_DISABLED
            MICRONES_SD_WARMUP_CS_LOW
            MICRONES_SD_INIT_DEBUG)
            if (DEFINED ${_sd_var})
                target_compile_definitions(${target_name} PRIVATE
                    ${_sd_var}=${${_sd_var}})
            endif ()
        endforeach ()

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
        elseif (audio_backend_name STREQUAL "hdmi")
            # HDMI audio rides on the same HSTX peripheral that emits video;
            # the backend wrapper just routes PCM into video_hstx.c.
            target_sources(${target_name} PRIVATE
                src/pico/pico_audio_backend_hdmi.c
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
                src/pico/display/tft_controller_ili9341.c
                src/pico/display/video_tft.c
            )
            target_compile_definitions(${target_name} PRIVATE MICRONES_PICO_VIDEO_BACKEND_TFT=1)
            # 250 MHz gives CPSDVSR=4, SCR=0 → exactly 62.5 MHz SPI (63.6 FPS ceiling).
            # 315 MHz gives best ≤62.5 MHz = 52.5 MHz (53.4 FPS ceiling) — wrong for TFT.
            # This overrides the #ifndef default in clock_config.h for this target only;
            # the analog target continues to use 315 MHz via the default.
            target_compile_definitions(${target_name} PRIVATE MICRONES_SYS_CLK_MHZ=250)

            if (MICRONES_PICO_TFT_BACKEND STREQUAL "spi_ili9341")
                if (NOT MICRONES_PICO_TFT_CONTROLLER STREQUAL "ili9341")
                    message(FATAL_ERROR "MICRONES_PICO_TFT_BACKEND=spi_ili9341 requires MICRONES_PICO_TFT_CONTROLLER=ili9341")
                endif ()

                target_sources(${target_name} PRIVATE
                    src/pico/display/display_spi_ili9341.c
                )
                target_link_libraries(${target_name} PRIVATE
                    hardware_spi
                )
                target_compile_definitions(${target_name} PRIVATE
                    MICRONES_DISPLAY_BACKEND_SPI_ILI9341=1
                    MICRONES_DISPLAY_CONTROLLER_ILI9341=1
                )
            elseif (MICRONES_PICO_TFT_BACKEND STREQUAL "parallel_8080")
                target_sources(${target_name} PRIVATE
                    src/pico/display/display_parallel_8080.c
                )
                target_link_libraries(${target_name} PRIVATE
                    hardware_dma
                    hardware_pio
                )
                target_compile_definitions(${target_name} PRIVATE
                    MICRONES_DISPLAY_BACKEND_PARALLEL_8080=1
                )
                pico_generate_pio_header(${target_name}
                    ${CMAKE_CURRENT_LIST_DIR}/src/pico/display/parallel_tft.pio
                )

                if (MICRONES_PICO_TFT_CONTROLLER STREQUAL "ili9341")
                    target_sources(${target_name} PRIVATE
                        src/pico/display/tft_controller_ili9341.c
                    )
                    target_compile_definitions(${target_name} PRIVATE
                        MICRONES_DISPLAY_CONTROLLER_ILI9341=1
                    )
                elseif (MICRONES_PICO_TFT_CONTROLLER STREQUAL "ili9486")
                    target_sources(${target_name} PRIVATE
                        src/pico/display/tft_controller_ili9486.c
                    )
                    target_compile_definitions(${target_name} PRIVATE
                        MICRONES_DISPLAY_CONTROLLER_ILI9486=1
                    )
                elseif (MICRONES_PICO_TFT_CONTROLLER STREQUAL "ili9488")
                    target_sources(${target_name} PRIVATE
                        src/pico/display/tft_controller_ili9488.c
                    )
                    target_compile_definitions(${target_name} PRIVATE
                        MICRONES_DISPLAY_CONTROLLER_ILI9488=1
                    )
                else ()
                    message(FATAL_ERROR "Unsupported MICRONES_PICO_TFT_CONTROLLER='${MICRONES_PICO_TFT_CONTROLLER}'")
                endif ()
            else ()
                message(FATAL_ERROR "Unsupported MICRONES_PICO_TFT_BACKEND='${MICRONES_PICO_TFT_BACKEND}'")
            endif ()

            if (MICRONES_PICO_VIDEO_MODE STREQUAL "emulator")
                target_sources(${target_name} PRIVATE
                    src/pico/emulator_video_adapter_tft.c
                )
            endif ()
        elseif (backend_name STREQUAL "hdmi")
            # Default: keep USB CDC stdio enabled for HDMI bring-up/debugging.
            # HSTX no longer borrows pll_usb, so USB can stay on while audio
            # uses an integer-derived clk_sys/2 HSTX clock.
            if (MICRONES_HDMI_STDIO_USB)
                pico_enable_stdio_usb(${target_name} 1)
                target_compile_definitions(${target_name} PRIVATE MICRONES_HDMI_STDIO_USB_ENABLED=1)
            else ()
                pico_enable_stdio_usb(${target_name} 0)
            endif ()
            pico_enable_stdio_uart(${target_name} 0)
            target_sources(${target_name} PRIVATE
                src/pico/hdmi_data_island.c
                src/pico/pico_video_backend_hdmi.c
                src/pico/video_hstx.c
            )
            target_link_libraries(${target_name} PRIVATE
                hardware_dma
            )
            target_compile_definitions(${target_name} PRIVATE
                MICRONES_PICO_VIDEO_BACKEND_HDMI=1
                MICRONES_BOARD_V0_1=1
                MICRONES_SYS_CLK_MHZ=315
            )
            # HDMI bring-up bisect switches (default to full HDMI mode):
            #   -DMICRONES_HDMI_DATA_ISLANDS=OFF   -> pure DVI in blanking
            #   -DMICRONES_HDMI_VIDEO_PREAMBLE=OFF -> drop video preamble + guard band
            #   -DMICRONES_HDMI_AUDIO_PACKETS=OFF  -> InfoFrames+ACR only, no PCM
            #   -DHDMI_BCH_POLY=0x83               -> alternative BCH polynomial
            if (DEFINED MICRONES_HDMI_DATA_ISLANDS AND NOT MICRONES_HDMI_DATA_ISLANDS)
                target_compile_definitions(${target_name} PRIVATE MICRONES_HDMI_DATA_ISLANDS=0)
            endif ()
            if (DEFINED MICRONES_HDMI_VIDEO_PREAMBLE AND NOT MICRONES_HDMI_VIDEO_PREAMBLE)
                target_compile_definitions(${target_name} PRIVATE MICRONES_HDMI_VIDEO_PREAMBLE=0)
            endif ()
            if (DEFINED MICRONES_HDMI_AUDIO_PACKETS AND NOT MICRONES_HDMI_AUDIO_PACKETS)
                target_compile_definitions(${target_name} PRIVATE MICRONES_HDMI_AUDIO_PACKETS=0)
            endif ()
            if (MICRONES_HDMI_TEST_TONE)
                target_compile_definitions(${target_name} PRIVATE MICRONES_HDMI_TEST_TONE=1)
            endif ()
            if (DEFINED HDMI_BCH_POLY)
                target_compile_definitions(${target_name} PRIVATE HDMI_BCH_POLY=${HDMI_BCH_POLY})
            endif ()

            if (MICRONES_PICO_VIDEO_MODE STREQUAL "emulator")
                target_sources(${target_name} PRIVATE
                    src/pico/emulator_video_adapter_hdmi.c
                )
            endif ()
        else ()
            message(FATAL_ERROR "Unknown Pico backend '${backend_name}'")
        endif ()

        pico_add_extra_outputs(${target_name})
    endfunction()

    micrones_add_pico_core(micrones_core_pico_analog 0)
    micrones_add_pico_core(micrones_core_pico_tft 1)
    micrones_add_pico_core(micrones_core_pico_hdmi 1)
    micrones_add_pico_target(micrones_pico_analog analog micrones_core_pico_analog pwm)
    micrones_add_pico_target(micrones_pico_tft tft micrones_core_pico_tft pwm)
    micrones_add_pico_target(micrones_pico_tft_max98357 tft micrones_core_pico_tft max98357)
    micrones_add_pico_target(micrones_pico_hdmi hdmi micrones_core_pico_hdmi hdmi)

    add_executable(micrones_pico_hdmi_test_pattern
        src/pico/hdmi_test_pattern.c
    )

    target_link_libraries(micrones_pico_hdmi_test_pattern PRIVATE
        pico_stdlib
        pico_multicore
        hardware_dma
        pico_sync
    )

    pico_add_extra_outputs(micrones_pico_hdmi_test_pattern)
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
