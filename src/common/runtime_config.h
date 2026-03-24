#ifndef MICRONES_RUNTIME_CONFIG_H
#define MICRONES_RUNTIME_CONFIG_H

#include <stdint.h>

// MICRONES_HOT_FUNC(name): place the function in SRAM to avoid flash XIP /
// ICache latency on the hottest emulator paths.
//   Pico:   __not_in_flash_func  (XIP SRAM)
//   ESP32:  .iram1.text section  (IRAM – no ICache, direct CPU access)
//   Other:  no-op
#ifndef MICRONES_HOT_FUNC
#ifdef MICRONES_PICO_PLATFORM
#include "pico.h"
#define MICRONES_HOT_FUNC(name) __not_in_flash_func(name)
#elif defined(MICRONES_ESP32_PLATFORM)
// Place in IRAM so the 6502/PPU/APU hot-loops never stall on ICache misses.
// Equivalent to ESP-IDF's IRAM_ATTR without requiring esp_attr.h here.
#define MICRONES_HOT_FUNC(name) \
    __attribute__((section(".iram1.text"))) name
#else
#define MICRONES_HOT_FUNC(name) name
#endif
#endif

/* APU engine selection: 0 = InfoNES DDS (default), 1 = cycle-accurate microNES APU */
#ifndef MICRONES_APU_ENGINE_MICRONES
#define MICRONES_APU_ENGINE_MICRONES 0
#endif

#ifndef MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
#define MICRONES_ENABLE_RUNTIME_DIAGNOSTICS 1
#endif

#ifndef MICRONES_ENABLE_APU_DEBUG_METRICS
#define MICRONES_ENABLE_APU_DEBUG_METRICS MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
#endif

#ifndef MICRONES_ENABLE_APU_PCM_OUTPUT
#define MICRONES_ENABLE_APU_PCM_OUTPUT 1
#endif

#ifndef MICRONES_ENABLE_APU_EMULATION
#define MICRONES_ENABLE_APU_EMULATION 1
#endif

#ifndef MICRONES_ENABLE_FRAMEBUFFER
#define MICRONES_ENABLE_FRAMEBUFFER 1
#endif

#ifndef MICRONES_ENABLE_STEP_PROFILING
#define MICRONES_ENABLE_STEP_PROFILING 0
#endif

#ifndef MICRONES_ENABLE_PICO_VIDEO_STATS
#define MICRONES_ENABLE_PICO_VIDEO_STATS 1
#endif

typedef uint64_t (*micrones_profile_now_us_fn)(void *user);

#endif
