#ifndef MICRONES_RUNTIME_CONFIG_H
#define MICRONES_RUNTIME_CONFIG_H

#include <stdint.h>

// MICRONES_HOT_FUNC(name): place the function in SRAM on Pico builds to avoid
// flash XIP latency on the hottest emulator paths. Falls back to a no-op on
// host builds.
#ifndef MICRONES_HOT_FUNC
#ifdef MICRONES_PICO_PLATFORM
#include "pico.h"
#define MICRONES_HOT_FUNC(name) __not_in_flash_func(name)
#else
#define MICRONES_HOT_FUNC(name) name
#endif
#endif

/* APU engine selection: 0 = InfoNES DDS (default), 1 = cycle-accurate legacy */
#ifndef MICRONES_APU_ENGINE_LEGACY
#define MICRONES_APU_ENGINE_LEGACY 0
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
