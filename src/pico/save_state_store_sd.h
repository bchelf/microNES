#ifndef MICRONES_PICO_SAVE_STATE_STORE_SD_H
#define MICRONES_PICO_SAVE_STATE_STORE_SD_H

#include "save_state_store.h"

#include <stdbool.h>

/* SD-card/FAT32-backed SaveStateStore.  Save states for a ROM live under
 * /SAVES/<8.3 ROM dir name>/<8-digit elapsed seconds>.SAV.
 *
 * Requires the SD card to already be mounted (e.g. via rom_source_sd_init(),
 * which calls fat32_mount()). */
bool save_state_store_sd_init(SaveStateStore *out);

#endif
