#ifndef MICRONES_PICO_ROM_SOURCE_FLASH_CACHE_H
#define MICRONES_PICO_ROM_SOURCE_FLASH_CACHE_H

#include "rom_source.h"

#include <stdbool.h>
#include <stddef.h>

/* Wrap another RomSource so load() copies the selected ROM into a reserved
 * flash range and returns the XIP pointer to that cached image. */
bool rom_source_flash_cache_init(RomSource *out_source, RomSource *backing);
const char *rom_source_flash_cache_last_error(void);

/* Optional progress callback invoked between flash erase sectors and program
 * pages.  Interrupts are enabled when this fires, so it is safe to present
 * video frames from within the callback. */
typedef void (*FlashCacheProgressFn)(size_t done, size_t total, void *user);
void rom_source_flash_cache_set_progress(FlashCacheProgressFn fn, void *user);

#endif
