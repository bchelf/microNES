#ifndef MICRONES_ROM_SOURCE_H
#define MICRONES_ROM_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* RomSource is a portable vtable.  The platform supplies an implementation
 * (POSIX directory scanner, SD/FatFs scanner, etc.) and the menu / app shell
 * drive it without knowing the underlying storage.
 *
 * Lifecycle:
 *   - The platform constructs a concrete source and hands a RomSource* to
 *     the app shell.
 *   - The app shell calls refresh() once at startup and may call it again
 *     in response to user action.
 *   - When the user picks an entry, the app shell calls load(), passes the
 *     bytes to the NES core (which copies them), then calls free_buf()
 *     immediately.
 *
 * The metadata in RomSourceEntry is populated at refresh() time so the menu
 * can dim entries whose mapper is unsupported without reading every file
 * twice. */

#define ROM_SOURCE_NAME_MAX 96

typedef struct {
    /* Display name for the menu (basename without trailing ".nes"). */
    char    name[ROM_SOURCE_NAME_MAX];
    /* iNES mapper number; 0xFFFF if the header could not be parsed. */
    uint16_t mapper;
    /* True when the running core can execute this ROM (see
     * rom_source_mapper_supported below). */
    bool    supported;
    /* True when iNES flags6 bit 1 indicates battery-backed PRG-RAM. */
    bool    has_battery;
    /* Decoded PRG/CHR sizes in bytes (0 if unknown). */
    uint32_t prg_size;
    uint32_t chr_size;
    /* Total file size in bytes (0 if unknown). */
    uint32_t file_size;
} RomSourceEntry;

typedef struct RomSource RomSource;
typedef bool (*RomSourceStreamWriteFn)(void *user, const uint8_t *data, size_t size);

struct RomSource {
    void *user;
    size_t (*count)(RomSource *self);
    const RomSourceEntry *(*entry)(RomSource *self, size_t index);

    /* Read the full ROM image for entry `index` into a freshly allocated
     * buffer.  On success, stores the buffer + size and returns true.  The
     * caller is responsible for calling free_buf() when done. */
    bool (*load)(RomSource *self, size_t index,
                 uint8_t **out_buf, size_t *out_size,
                 char *err, size_t err_size);

    /* Optional zero-copy-friendly load path.  The source streams the selected
     * ROM into `write` without allocating a full image.  On success, out_size
     * receives the ROM size.  Sources may leave this NULL; callers then fall
     * back to load(). */
    bool (*load_stream)(RomSource *self, size_t index,
                        RomSourceStreamWriteFn write, void *write_user,
                        size_t *out_size,
                        char *err, size_t err_size);

    /* Free a buffer previously returned by load(). */
    void (*free_buf)(RomSource *self, uint8_t *buf);

    /* Re-scan the underlying storage.  May be NULL for sources that never
     * change. */
    void (*refresh)(RomSource *self);

    /* Battery-backed PRG-RAM persistence.  Both functions are optional and
     * may be NULL.  If has_battery is true for an entry, the app shell
     * calls save_load() before run and save_store() on exit-to-menu. */
    bool (*save_load)(RomSource *self, size_t index,
                      uint8_t *wram, size_t wram_size);
    bool (*save_store)(RomSource *self, size_t index,
                       const uint8_t *wram, size_t wram_size);
};

/* Single source of truth for which mappers the core implements.  Used at
 * refresh() time by platform sources to populate entry->supported. */
static inline bool rom_source_mapper_supported(uint16_t mapper) {
    return mapper == 0u || mapper == 1u;
}

/* Construct a no-op RomSource that exposes zero entries and rejects loads.
 * Useful as a fallback when storage init fails so the menu still renders
 * an "empty" state. */
void rom_source_make_empty(RomSource *out);

#endif
