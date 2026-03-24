#ifndef MICRONES_APU_H
#define MICRONES_APU_H

/*
 * apu.h — APU engine selector
 *
 * Set MICRONES_APU_ENGINE_MICRONES=1 (via CMake option MICRONES_APU_ENGINE=micrones)
 * to build the original cycle-accurate microNES APU.  The default
 * (MICRONES_APU_ENGINE=infones) uses the InfoNES DDS synthesis engine.
 */

#if MICRONES_APU_ENGINE_MICRONES
#include "apu_micrones.h"
#else
#include "apu_infones.h"
#endif

#endif /* MICRONES_APU_H */
