#ifndef MICRONES_HOST_ROM_SOURCE_POSIX_H
#define MICRONES_HOST_ROM_SOURCE_POSIX_H

#include "rom_source.h"

#include <stdbool.h>

/* Construct a RomSource backed by a POSIX directory (one flat level, *.nes
 * only).  Returns true on success.  The returned RomSource owns heap state
 * which must be released with rom_source_posix_destroy().
 *
 * The implementation pre-parses iNES headers at refresh time so the menu
 * can dim unsupported entries without re-reading every file. */
bool rom_source_posix_init(RomSource *out_source, const char *dir_path);
void rom_source_posix_destroy(RomSource *source);

#endif
