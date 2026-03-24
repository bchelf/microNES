cmake_minimum_required(VERSION 3.16)

# Path to the portable NES core (relative to this CMakeLists.txt)
set(MICRONES_COMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../../common")

# ROM lives at roms/smb1.nes in the repository root.
# From src/esp32s3/main/ that is three directories up.
set(MICRONES_ROM_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../roms/smb1.nes")

if(EXISTS "${MICRONES_ROM_PATH}")
    set(MICRONES_HAS_ROM_FLAG 1)
    message(STATUS "microNES: embedding ${MICRONES_ROM_PATH}")
else()
    set(MICRONES_HAS_ROM_FLAG 0)
    message(WARNING "microNES: roms/smb1.nes not found in repo root – build will succeed but the emulator won't run. Add the ROM at roms/smb1.nes and rebuild.")
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
)

# Embed the ROM binary using target_add_binary_data, which accepts absolute paths.
if(MICRONES_HAS_ROM_FLAG)
    target_add_binary_data(${COMPONENT_LIB} "${MICRONES_ROM_PATH}" BINARY)
endif()

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
