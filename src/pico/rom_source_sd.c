#include "rom_source_sd.h"

#include "fat32.h"
#include "sd_spi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cap the visible ROM list so the entry table fits comfortably in SRAM.
 * 128 entries × ~120 bytes = ~15 KB, leaving plenty of room for the rest
 * of the runtime. */
enum {
    SD_ROM_LIST_MAX = 128,
};

typedef struct {
    RomSourceEntry meta;
    uint32_t       first_cluster;
    uint32_t       size;
} SdRomEntry;

typedef struct {
    SdRomEntry entries[SD_ROM_LIST_MAX];
    size_t     entry_count;
} SdState;

static SdState s_sd_state;
static char    s_last_error[160];

static void set_error(const char *msg) {
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg);
}

const char *rom_source_sd_last_error(void) {
    return s_last_error;
}

static bool ends_with_nes_ci(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return false;
    const char *s = &name[n - 4];
    return s[0] == '.' &&
           (s[1] == 'n' || s[1] == 'N') &&
           (s[2] == 'e' || s[2] == 'E') &&
           (s[3] == 's' || s[3] == 'S');
}

static void strip_extension(char *name) {
    size_t n = strlen(name);
    if (n >= 4 && name[n - 4] == '.') {
        name[n - 4] = '\0';
    }
}

static void parse_ines_header(const uint8_t hdr[16], RomSourceEntry *meta) {
    meta->mapper = 0xFFFFu;
    meta->supported = false;
    meta->has_battery = false;
    meta->prg_size = 0;
    meta->chr_size = 0;

    if (memcmp(hdr, "NES\x1a", 4) != 0) {
        return;
    }
    uint32_t prg_banks = hdr[4];
    uint32_t chr_banks = hdr[5];
    uint8_t flags6 = hdr[6];
    uint8_t flags7 = hdr[7];
    bool is_nes2 = (flags7 & 0x0Cu) == 0x08u;

    uint32_t mapper = (uint32_t)(flags6 >> 4) | (uint32_t)(flags7 & 0xF0u);
    if (is_nes2) {
        mapper |= (uint32_t)(hdr[8] & 0x0Fu) << 8;
        prg_banks |= (uint32_t)(hdr[9] & 0x0Fu) << 8;
        chr_banks |= (uint32_t)(hdr[9] >> 4) << 8;
    }

    meta->mapper      = (uint16_t)(mapper & 0xFFFFu);
    meta->has_battery = (flags6 & 0x02u) != 0;
    meta->prg_size    = prg_banks * 16u * 1024u;
    meta->chr_size    = chr_banks * 8u * 1024u;
    meta->supported   = rom_source_mapper_supported(meta->mapper);
}

static int compare_name(const void *a, const void *b) {
    const SdRomEntry *ea = (const SdRomEntry *)a;
    const SdRomEntry *eb = (const SdRomEntry *)b;
    return strcmp(ea->meta.name, eb->meta.name);
}

static bool name_equals_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void log_fat_entry(const char *dir_name, const Fat32Entry *entry) {
    const bool is_dir = (entry->attr & FAT32_ATTR_DIRECTORY) != 0u;
    printf("SD scan: %s/%s %s size=%lu cluster=%lu attr=0x%02x\n",
           dir_name,
           entry->name,
           is_dir ? "dir" : "file",
           (unsigned long)entry->size,
           (unsigned long)entry->first_cluster,
           (unsigned)entry->attr);
}

/* Walk one directory and append its .nes files to st->entries.  If
 * out_roms_cluster is non-NULL, also looks for a subdirectory named
 * "roms" (case-insensitive) and writes its first cluster there. */
static void scan_dir_for_nes(SdState *st, Fat32DirIter *it,
                             const char *dir_name,
                             uint32_t *out_roms_cluster) {
    Fat32Entry e;
    bool capped_logged = false;
    while (fat32_dir_next(it, &e)) {
        log_fat_entry(dir_name, &e);
        if ((e.attr & FAT32_ATTR_DIRECTORY) != 0u) {
            /* Note the /roms subdirectory but don't recurse from here. */
            if (out_roms_cluster != NULL && *out_roms_cluster == 0u &&
                name_equals_ci(e.name, "roms")) {
                *out_roms_cluster = e.first_cluster;
                printf("SD scan: found /roms at cluster=%lu\n",
                       (unsigned long)e.first_cluster);
            }
            continue;
        }
        if (!ends_with_nes_ci(e.name)) continue;
        if (st->entry_count >= SD_ROM_LIST_MAX) {
            if (!capped_logged) {
                printf("SD scan: ROM list capped at %u entries; continuing directory log only\n",
                       (unsigned)SD_ROM_LIST_MAX);
                capped_logged = true;
            }
            continue;
        }

        SdRomEntry *entry = &st->entries[st->entry_count];
        memset(entry, 0, sizeof(*entry));

        snprintf(entry->meta.name, sizeof(entry->meta.name), "%.*s",
                 (int)sizeof(entry->meta.name) - 1, e.name);
        strip_extension(entry->meta.name);
        entry->meta.file_size = e.size;
        entry->first_cluster  = e.first_cluster;
        entry->size           = e.size;

        /* Read the iNES header to populate metadata.  Skip pathological
         * empty/short files. */
        if (e.size >= 16u) {
            Fat32File f;
            fat32_open_file(&f, &e);
            uint8_t hdr[16];
            if (fat32_read(&f, hdr, sizeof(hdr)) == sizeof(hdr)) {
                parse_ines_header(hdr, &entry->meta);
                printf("SD scan: %s/%s iNES mapper=%u supported=%u prg=%lu chr=%lu battery=%u\n",
                       dir_name,
                       e.name,
                       (unsigned)entry->meta.mapper,
                       entry->meta.supported ? 1u : 0u,
                       (unsigned long)entry->meta.prg_size,
                       (unsigned long)entry->meta.chr_size,
                       entry->meta.has_battery ? 1u : 0u);
            } else {
                printf("SD scan: %s/%s iNES header read failed\n", dir_name, e.name);
            }
        } else {
            printf("SD scan: %s/%s skipped, file shorter than iNES header\n",
                   dir_name,
                   e.name);
        }

        ++st->entry_count;
    }
}

static void sd_refresh(RomSource *self) {
    SdState *st = (SdState *)self->user;
    st->entry_count = 0;

    /* Re-mount in case the card was changed.  Cheap if state is the same. */
    if (!fat32_is_mounted()) {
        if (sd_init() != SD_OK) {
            set_error("SD init failed");
            return;
        }
        if (!fat32_mount()) {
            set_error("FAT32 mount failed");
            return;
        }
    }

    /* Pass 1: root directory.  Collects .nes files at the top level and
     * notes the first cluster of any subdirectory named "roms". */
    Fat32DirIter it;
    uint32_t roms_cluster = 0u;
    if (fat32_open_root(&it)) {
        printf("SD scan: reading root directory\n");
        scan_dir_for_nes(st, &it, "root", &roms_cluster);
    } else {
        set_error("Failed to open root directory");
        return;
    }

    /* Pass 2: /roms subdirectory if present.  No recursion past one
     * level — anything else is intentionally ignored. */
    if (roms_cluster != 0u && fat32_open_dir_at_cluster(&it, roms_cluster)) {
        printf("SD scan: reading /roms directory\n");
        scan_dir_for_nes(st, &it, "roms", NULL);
    }

    qsort(st->entries, st->entry_count, sizeof(*st->entries), compare_name);
    printf("SD scan: complete, %u ROM(s)\n", (unsigned)st->entry_count);
    set_error("");
}

static size_t sd_count(RomSource *self) {
    SdState *st = (SdState *)self->user;
    return st->entry_count;
}

static const RomSourceEntry *sd_entry(RomSource *self, size_t index) {
    SdState *st = (SdState *)self->user;
    if (index >= st->entry_count) return NULL;
    return &st->entries[index].meta;
}

static bool sd_load(RomSource *self, size_t index,
                    uint8_t **out_buf, size_t *out_size,
                    char *err, size_t err_size) {
    SdState *st = (SdState *)self->user;
    if (index >= st->entry_count) {
        if (err && err_size) snprintf(err, err_size, "index out of range");
        return false;
    }
    SdRomEntry *e = &st->entries[index];

    printf("SD load: reading \"%s\" size=%lu cluster=%lu\n",
           e->meta.name,
           (unsigned long)e->size,
           (unsigned long)e->first_cluster);

    uint8_t *buf = (uint8_t *)malloc(e->size);
    if (buf == NULL) {
        if (err && err_size) snprintf(err, err_size, "out of memory (%u bytes)",
                                       (unsigned)e->size);
        return false;
    }

    Fat32Entry fe;
    memset(&fe, 0, sizeof(fe));
    fe.first_cluster = e->first_cluster;
    fe.size          = e->size;

    Fat32File f;
    fat32_open_file(&f, &fe);

    size_t got = fat32_read(&f, buf, e->size);
    if (got != e->size) {
        free(buf);
        printf("SD load: \"%s\" failed read=%lu/%lu\n",
               e->meta.name,
               (unsigned long)got,
               (unsigned long)e->size);
        if (err && err_size) snprintf(err, err_size, "read %u/%u bytes",
                                       (unsigned)got, (unsigned)e->size);
        return false;
    }

    printf("SD load: \"%s\" read complete bytes=%lu\n",
           e->meta.name,
           (unsigned long)got);
    *out_buf  = buf;
    *out_size = e->size;
    return true;
}

static void sd_free_buf(RomSource *self, uint8_t *buf) {
    (void)self;
    free(buf);
}

bool rom_source_sd_init(RomSource *out_source) {
    if (out_source == NULL) return false;

    s_last_error[0] = '\0';
    memset(&s_sd_state, 0, sizeof(s_sd_state));

    printf("SD source: initializing card\n");
    if (sd_init() != SD_OK) {
        set_error("SD card init failed");
        return false;
    }
    printf("SD source: mounting FAT32 volume\n");
    if (!fat32_mount()) {
        set_error("No FAT32 partition");
        return false;
    }

    memset(out_source, 0, sizeof(*out_source));
    out_source->user      = &s_sd_state;
    out_source->count     = sd_count;
    out_source->entry     = sd_entry;
    out_source->load      = sd_load;
    out_source->free_buf  = sd_free_buf;
    out_source->refresh   = sd_refresh;
    /* save_load / save_store left NULL — write-side FAT32 is a follow-up. */

    sd_refresh(out_source);
    return true;
}

void rom_source_sd_destroy(RomSource *source) {
    if (source == NULL) return;
    memset(source, 0, sizeof(*source));
    memset(&s_sd_state, 0, sizeof(s_sd_state));
}
