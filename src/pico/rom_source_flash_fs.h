#ifndef MICRONES_PICO_ROM_SOURCE_FLASH_FS_H
#define MICRONES_PICO_ROM_SOURCE_FLASH_FS_H

#include "rom_source.h"

#include <stdbool.h>
#include <stddef.h>

typedef void (*FlashFsProgressFn)(size_t done, size_t total, void *user);

bool rom_source_flash_fs_init(RomSource *out_source);
const char *rom_source_flash_fs_last_error(void);

void rom_source_flash_fs_set_progress(FlashFsProgressFn fn, void *user);

bool rom_source_flash_fs_erase(RomSource *self);
bool rom_source_flash_fs_copy_from(RomSource *self, RomSource *sd_source);

#endif
