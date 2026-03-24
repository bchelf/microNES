#ifndef MICRONES_APU_H
#define MICRONES_APU_H

/*
 * apu.h — APU engine selector
 *
 * Set MICRONES_APU_ENGINE_LEGACY=1 (via CMake option MICRONES_APU_ENGINE=legacy)
 * to build the original cycle-accurate APU.  The default (MICRONES_APU_ENGINE=infones)
 * uses the InfoNES DDS synthesis engine.
 */

#if MICRONES_APU_ENGINE_LEGACY
#include "apu_legacy.h"
#else
#include "apu_infones.h"
#endif

#endif /* MICRONES_APU_H */
