cmake_minimum_required(VERSION 3.16)

# Path to the portable NES core (relative to this CMakeLists.txt)
set(MICRONES_COMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../../common")

# ROM lives at roms/smb1.nes in the repository root.
# From src/esp32s3/main/ that is three directories up.
set(MICRONES_ROM_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../roms/smb1.nes")

set(MICRONES_APU_ENGINE "infones" CACHE STRING "APU engine for ESP32-S3 target (infones or micrones)")
set_property(CACHE MICRONES_APU_ENGINE PROPERTY STRINGS infones micrones)

if(MICRONES_APU_ENGINE STREQUAL "micrones")
    set(MICRONES_APU_SOURCE "${MICRONES_COMMON_DIR}/apu_micrones.c")
    set(MICRONES_APU_ENGINE_FLAG 1)
elseif(MICRONES_APU_ENGINE STREQUAL "infones")
    set(MICRONES_APU_SOURCE "${MICRONES_COMMON_DIR}/apu_infones.c")
    set(MICRONES_APU_ENGINE_FLAG 0)
else()
    message(FATAL_ERROR "MICRONES_APU_ENGINE must be 'infones' or 'micrones'")
endif()

message(STATUS "microNES ESP32-S3 APU engine: ${MICRONES_APU_ENGINE}")

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
        "${MICRONES_APU_SOURCE}"
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
    MICRONES_APU_ENGINE_MICRONES=${MICRONES_APU_ENGINE_FLAG}
    MICRONES_ENABLE_RUNTIME_DIAGNOSTICS=0
    MICRONES_ENABLE_APU_DEBUG_METRICS=0
    MICRONES_ENABLE_APU_PCM_OUTPUT=1
    MICRONES_ENABLE_APU_EMULATION=1
    MICRONES_ENABLE_FRAMEBUFFER=1
    MICRONES_ENABLE_SCANLINE_BUFFER=0
    MICRONES_ENABLE_STEP_PROFILING=0
    MICRONES_ENABLE_CYCLE_PROFILING=0
    # Activates IRAM placement for cpu6502_step / ppu_render_scanline / apu_step etc.
    # Those functions run in SRAM instead of flash-backed ICache, eliminating
    # cache-miss stalls in the tight 6502/PPU dispatch loops.
    MICRONES_ESP32_PLATFORM=1
    # ROM embedding flag
    MICRONES_HAS_ROM=${MICRONES_HAS_ROM_FLAG}
)

# Compile everything in this component at -O3.
# ESP-IDF sdkconfig.defaults sets CONFIG_COMPILER_OPTIMIZATION_PERF=y (-O2)
# globally.  The per-target override here raises only this component to -O3,
# which noticeably tightens the cpu6502/PPU inner loops.
#
# -mtext-section-literals (Xtensa-specific, CRITICAL for IRAM correctness):
#   On Xtensa LX7, L32R loads a 32-bit literal from a pool that must be within
#   ±256 KB of the instruction.  Without this flag the literal pools go to a
#   separate .literal section which the default linker script maps to flash
#   (~0x42000000), while IRAM-tagged functions live at ~0x40374000 – a 28 MB
#   gap that causes silent L32R range faults.  This flag moves every literal
#   pool inline inside the owning function's text section, so when a function
#   is placed in .iram1.text its constants follow it there automatically.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-O3 -mtext-section-literals>
)

# Link math library (required by micrones APU)
target_link_libraries(${COMPONENT_LIB} PUBLIC m)
