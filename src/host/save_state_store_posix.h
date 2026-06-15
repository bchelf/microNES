#ifndef MICRONES_HOST_SAVE_STATE_STORE_POSIX_H
#define MICRONES_HOST_SAVE_STATE_STORE_POSIX_H

#include "save_state_store.h"

#include <stdbool.h>

/* Construct a SaveStateStore backed by POSIX directories under base_dir.
 * Save states for a ROM named "Foo Bar" live under
 * base_dir/<8.3 dir name>/<elapsed seconds>.SAV (see
 * save_state_store_dir_name / save_state_store_file_name).
 *
 * base_dir is created (mkdir) if it does not already exist; per-ROM
 * subdirectories are created lazily by save().  Returns true on success.
 * The returned store owns heap state which must be released with
 * save_state_store_posix_destroy(). */
bool save_state_store_posix_init(SaveStateStore *out, const char *base_dir);
void save_state_store_posix_destroy(SaveStateStore *store);

#endif
