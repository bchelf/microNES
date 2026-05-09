#include "rom_source_posix.h"

#include "rom_source.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum {
    POSIX_DIR_MAX     = 1024,
    POSIX_NAME_MAX    = 255,
    POSIX_PATH_MAX    = POSIX_DIR_MAX + 1 + POSIX_NAME_MAX + 4 + 1, /* dir + '/' + name + ".sav" + NUL */
};

typedef struct {
    char            full_path[POSIX_PATH_MAX];   /* /dir/name.nes */
    char            save_path[POSIX_PATH_MAX];   /* /dir/name.sav */
    RomSourceEntry  meta;
} PosixEntry;

typedef struct {
    char        dir_path[POSIX_DIR_MAX];
    PosixEntry *entries;
    size_t      entry_count;
    size_t      entry_capacity;
} PosixState;

static bool ends_with_nes_ci(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return false;
    const char *suffix = &name[n - 4];
    return (suffix[0] == '.') &&
           (suffix[1] == 'n' || suffix[1] == 'N') &&
           (suffix[2] == 'e' || suffix[2] == 'E') &&
           (suffix[3] == 's' || suffix[3] == 'S');
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

    meta->mapper = (uint16_t)(mapper & 0xFFFFu);
    meta->has_battery = (flags6 & 0x02u) != 0;
    meta->prg_size = prg_banks * 16u * 1024u;
    meta->chr_size = chr_banks * 8u * 1024u;
    meta->supported = rom_source_mapper_supported(meta->mapper);
}

static void strip_extension(char *name) {
    size_t n = strlen(name);
    if (n >= 4 && (name[n - 4] == '.')) {
        name[n - 4] = '\0';
    }
}

static int posix_compare_name(const void *a, const void *b) {
    const PosixEntry *ea = (const PosixEntry *)a;
    const PosixEntry *eb = (const PosixEntry *)b;
    return strcmp(ea->meta.name, eb->meta.name);
}

static bool posix_state_grow(PosixState *st) {
    size_t new_cap = st->entry_capacity == 0 ? 32 : st->entry_capacity * 2;
    PosixEntry *grown = (PosixEntry *)realloc(st->entries, new_cap * sizeof(*grown));
    if (grown == NULL) return false;
    st->entries = grown;
    st->entry_capacity = new_cap;
    return true;
}

static void posix_state_clear(PosixState *st) {
    free(st->entries);
    st->entries = NULL;
    st->entry_count = 0;
    st->entry_capacity = 0;
}

static void posix_refresh(RomSource *self) {
    PosixState *st = (PosixState *)self->user;

    posix_state_clear(st);

    DIR *dir = opendir(st->dir_path);
    if (dir == NULL) {
        return;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!ends_with_nes_ci(de->d_name)) continue;

        if (st->entry_count == st->entry_capacity) {
            if (!posix_state_grow(st)) break;
        }

        PosixEntry *entry = &st->entries[st->entry_count];
        memset(entry, 0, sizeof(*entry));

        /* Precision specifiers cap the inputs so static analyzers can prove
         * the snprintf cannot truncate.  POSIX d_name is at most 255 chars
         * and dir_path is bounded by POSIX_DIR_MAX. */
        snprintf(entry->full_path, sizeof(entry->full_path),
                 "%.*s/%.*s",
                 (int)POSIX_DIR_MAX - 1, st->dir_path,
                 (int)POSIX_NAME_MAX, de->d_name);

        snprintf(entry->meta.name, sizeof(entry->meta.name),
                 "%.*s", (int)sizeof(entry->meta.name) - 1, de->d_name);
        strip_extension(entry->meta.name);

        /* Build sibling .sav path. */
        snprintf(entry->save_path, sizeof(entry->save_path),
                 "%.*s/%.*s.sav",
                 (int)POSIX_DIR_MAX - 1, st->dir_path,
                 (int)sizeof(entry->meta.name) - 1, entry->meta.name);

        /* Read 16-byte iNES header for metadata. */
        FILE *fp = fopen(entry->full_path, "rb");
        if (fp != NULL) {
            uint8_t hdr[16];
            size_t got = fread(hdr, 1, sizeof(hdr), fp);

            struct stat sb;
            if (fstat(fileno(fp), &sb) == 0 && sb.st_size >= 0) {
                entry->meta.file_size = (uint32_t)sb.st_size;
            }
            fclose(fp);

            if (got == sizeof(hdr)) {
                parse_ines_header(hdr, &entry->meta);
            }
        }

        ++st->entry_count;
    }
    closedir(dir);

    qsort(st->entries, st->entry_count, sizeof(*st->entries), posix_compare_name);
}

static size_t posix_count(RomSource *self) {
    PosixState *st = (PosixState *)self->user;
    return st->entry_count;
}

static const RomSourceEntry *posix_entry(RomSource *self, size_t index) {
    PosixState *st = (PosixState *)self->user;
    if (index >= st->entry_count) return NULL;
    return &st->entries[index].meta;
}

static bool posix_load(RomSource *self, size_t index,
                       uint8_t **out_buf, size_t *out_size,
                       char *err, size_t err_size) {
    PosixState *st = (PosixState *)self->user;
    if (index >= st->entry_count) {
        if (err && err_size) snprintf(err, err_size, "index out of range");
        return false;
    }
    PosixEntry *e = &st->entries[index];

    FILE *fp = fopen(e->full_path, "rb");
    if (fp == NULL) {
        if (err && err_size) snprintf(err, err_size, "fopen failed for %s", e->full_path);
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        if (err && err_size) snprintf(err, err_size, "fseek failed");
        return false;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        if (err && err_size) snprintf(err, err_size, "empty file");
        return false;
    }
    rewind(fp);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (buf == NULL) {
        fclose(fp);
        if (err && err_size) snprintf(err, err_size, "out of memory");
        return false;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        if (err && err_size) snprintf(err, err_size, "fread failed");
        return false;
    }
    fclose(fp);

    *out_buf = buf;
    *out_size = (size_t)sz;
    return true;
}

static void posix_free_buf(RomSource *self, uint8_t *buf) {
    (void)self;
    free(buf);
}

static bool posix_save_load(RomSource *self, size_t index,
                            uint8_t *wram, size_t wram_size) {
    PosixState *st = (PosixState *)self->user;
    if (index >= st->entry_count) return false;
    PosixEntry *e = &st->entries[index];

    FILE *fp = fopen(e->save_path, "rb");
    if (fp == NULL) return false;
    size_t got = fread(wram, 1, wram_size, fp);
    fclose(fp);
    return got == wram_size;
}

static bool posix_save_store(RomSource *self, size_t index,
                             const uint8_t *wram, size_t wram_size) {
    PosixState *st = (PosixState *)self->user;
    if (index >= st->entry_count) return false;
    PosixEntry *e = &st->entries[index];

    FILE *fp = fopen(e->save_path, "wb");
    if (fp == NULL) return false;
    size_t wrote = fwrite(wram, 1, wram_size, fp);
    fclose(fp);
    return wrote == wram_size;
}

bool rom_source_posix_init(RomSource *out_source, const char *dir_path) {
    if (out_source == NULL || dir_path == NULL) return false;

    PosixState *st = (PosixState *)calloc(1, sizeof(*st));
    if (st == NULL) return false;

    snprintf(st->dir_path, sizeof(st->dir_path), "%.*s",
             (int)sizeof(st->dir_path) - 1, dir_path);
    /* Strip trailing slash for cleaner concatenation. */
    size_t n = strlen(st->dir_path);
    while (n > 1 && st->dir_path[n - 1] == '/') {
        st->dir_path[--n] = '\0';
    }

    out_source->user = st;
    out_source->count = posix_count;
    out_source->entry = posix_entry;
    out_source->load = posix_load;
    out_source->free_buf = posix_free_buf;
    out_source->refresh = posix_refresh;
    out_source->save_load = posix_save_load;
    out_source->save_store = posix_save_store;

    return true;
}

void rom_source_posix_destroy(RomSource *source) {
    if (source == NULL || source->user == NULL) return;
    PosixState *st = (PosixState *)source->user;
    posix_state_clear(st);
    free(st);
    memset(source, 0, sizeof(*source));
}
