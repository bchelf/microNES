#ifndef MICRONES_SAVE_STATE_STORE_H
#define MICRONES_SAVE_STATE_STORE_H

#include "save_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Portable vtable for save-state storage, modeled on RomSource.  A platform
 * supplies a concrete implementation (POSIX directories on the host, FAT32/SD
 * on the Pico) and the app shell drives it without knowing the underlying
 * storage.
 *
 * Save states for a given ROM live together under a directory derived from
 * the ROM's display name (see save_state_store_dir_name) and are named after
 * their elapsed-time (see save_state_store_file_name), so the on-disk layout
 * is a flat list of "<elapsed seconds>.SAV" files per ROM.
 *
 * Lifecycle:
 *   - refresh(rom_name) re-scans the save states for one ROM.  count()/
 *     entry()/load()/save()/clear_all() all operate on whichever ROM was
 *     named in the most recent refresh() call.
 *   - entries are ordered newest-first (entry 0 = highest elapsed_seconds,
 *     i.e. the most recently created save). */

enum {
    /* Cap on save states listed per ROM.  Generous for a handheld menu;
     * keeps the cached entry table small on memory-constrained targets. */
    SAVE_STATE_STORE_MAX_ENTRIES = 64,
};

typedef struct {
    uint32_t elapsed_seconds;
    /* "MM:SS" (or longer once minutes exceed 99), see
     * save_state_format_elapsed(). */
    char     label[12];
} SaveStateEntry;

typedef struct SaveStateStore SaveStateStore;

struct SaveStateStore {
    void *user;

    /* Re-scan save states for rom_name (a display name, e.g.
     * RomSourceEntry.name -- not yet sanitized to 8.3).  Returns the number
     * of save states found (0 on error or if none exist). */
    size_t (*refresh)(SaveStateStore *self, const char *rom_name);

    size_t (*count)(SaveStateStore *self);

    /* Entries are ordered newest-first.  Returns NULL if index is out of
     * range. */
    const SaveStateEntry *(*entry)(SaveStateStore *self, size_t index);

    /* Load the save-state blob at index into *out.  Returns false on error;
     * *out is left untouched on failure. */
    bool (*load)(SaveStateStore *self, size_t index, SaveStateBlob *out);

    /* Write a new save-state blob for the ROM named in the most recent
     * refresh() call.  On success, re-scans so count()/entry() reflect the
     * new state.  If writing collides with an existing save's filename,
     * blob->header.elapsed_seconds (and its crc32) are updated in place to
     * match the elapsed time actually recorded on disk, so callers should
     * re-read blob->header.elapsed_seconds afterward rather than assuming
     * it is unchanged. */
    bool (*save)(SaveStateStore *self, SaveStateBlob *blob);

    /* Delete all save states for the ROM named in the most recent refresh()
     * call.  Returns true on success, including when there was nothing to
     * delete.  On success, count() becomes 0. */
    bool (*clear_all)(SaveStateStore *self);
};

/* Derive an 8.3-safe, uppercase directory name from a ROM's display name:
 * keeps only [A-Z0-9_-], uppercases letters, and truncates to 8 characters.
 * Falls back to "_" if the name contains no eligible characters.  out must
 * be at least 9 bytes. */
static inline void save_state_store_dir_name(const char *rom_name, char out[9]) {
    int n = 0;
    if (rom_name != NULL) {
        for (const char *p = rom_name; *p != '\0' && n < 8; ++p) {
            char c = *p;
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-') {
                out[n++] = c;
            }
        }
    }
    if (n == 0) {
        out[n++] = '_';
    }
    out[n] = '\0';
}

/* Format an 8.3 short filename "<elapsed seconds, zero-padded to 8
 * digits>.SAV" for a save state.  elapsed_seconds is taken modulo 10^8 so
 * the digit count never exceeds the 8.3 name field.  out must be at least
 * 13 bytes. */
static inline void save_state_store_file_name(uint32_t elapsed_seconds, char out[13]) {
    snprintf(out, 13, "%08u.SAV", (unsigned)(elapsed_seconds % 100000000u));
}

#endif
