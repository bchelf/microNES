cmake_minimum_required(VERSION 3.16)

# Path to the portable NES core (relative to this CMakeLists.txt)
set(MICRONES_COMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../../common")

# Optionally embed a ROM: place rom.nes alongside this file before building.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/rom.nes")
    set(EMBED_ROM_FILES EMBED_FILES "rom.nes")
    set(MICRONES_HAS_ROM_FLAG 1)
    message(STATUS "microNES: embedding rom.nes")
else()
    set(EMBED_ROM_FILES "")
    set(MICRONES_HAS_ROM_FLAG 0)
    message(WARNING "microNES: rom.nes not found in ${CMAKE_CURRENT_LIST_DIR} – build will succeed but the emulator won't run without a ROM. Copy your NES ROM to src/esp32s3/main/rom.nes and rebuild.")
endif()

idf_component_register(
    SRCS
        # Portable NES emulator core
        "${MICRONES_COMMON_DIR}/apu_micrones.c"
        "${MICRONES_COMMON_DIR}/cart.c"
        "${MICRONES_COMMON_DIR}/cpu6502.c"
        "${MICRONES_COMMON_DIR}/cpu6502_opcode.c"
        "${MICRONES_COMMON_DIR}/frame_pacer.c"
        "${MICRONES_COMMON_DIR}/input.c"
        "${MICRONES_COMMON_DIR}/nes.c"
        "${MICRONES_COMMON_DIR}/nrom.c"
        "${MICRONES_COMMON_DIR}/ppu.c"
        # ESP32-S3 frontend
        "main.c"
        "display.c"
        "touch.c"
        "audio.c"
        "nes_input.c"
        "nes_video.c"
        "ui.c"
    INCLUDE_DIRS
        "."
        "${MICRONES_COMMON_DIR}"
    REQUIRES
        driver
        esp_timer
        esp_lcd
        nvs_flash
        log
    ${EMBED_ROM_FILES}
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    # NES core flags
    MICRONES_APU_ENGINE_MICRONES=1
    MICRONES_ENABLE_RUNTIME_DIAGNOSTICS=0
    MICRONES_ENABLE_APU_DEBUG_METRICS=0
    MICRONES_ENABLE_APU_PCM_OUTPUT=1
    MICRONES_ENABLE_APU_EMULATION=1
    MICRONES_ENABLE_FRAMEBUFFER=1
    MICRONES_ENABLE_STEP_PROFILING=0
    # ROM embedding flag
    MICRONES_HAS_ROM=${MICRONES_HAS_ROM_FLAG}
)

# Link math library (required by micrones APU)
target_link_libraries(${COMPONENT_LIB} PUBLIC m)
