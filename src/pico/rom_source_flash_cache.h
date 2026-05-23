#ifndef MICRONES_PICO_ROM_SOURCE_FLASH_CACHE_H
#define MICRONES_PICO_ROM_SOURCE_FLASH_CACHE_H

#include "rom_source.h"

#include <stdbool.h>

/* Wrap another RomSource so load() copies the selected ROM into a reserved
 * flash range and returns the XIP pointer to that cached image. */
bool rom_source_flash_cache_init(RomSource *out_source, RomSource *backing);
const char *rom_source_flash_cache_last_error(void);

#endif
