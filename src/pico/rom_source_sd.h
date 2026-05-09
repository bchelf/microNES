#ifndef MICRONES_PICO_ROM_SOURCE_SD_H
#define MICRONES_PICO_ROM_SOURCE_SD_H

#include "rom_source.h"

#include <stdbool.h>

/* Initialize an SD-card-backed RomSource.  The card must be present and
 * the FAT32 volume must mount; on failure returns false and the source
 * is left zeroed.  Pre-scans the root directory and parses iNES headers
 * so the menu can render mapper info immediately. */
bool rom_source_sd_init(RomSource *out_source);
void rom_source_sd_destroy(RomSource *source);

/* Last error string from a failed init, for reporting. */
const char *rom_source_sd_last_error(void);

#endif
