#include "save_state_store_posix.h"

#include <ctype.h>
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

enum {
    POSIX_SS_DIR_MAX = 1024,
};

typedef struct {
    char           base_dir[POSIX_SS_DIR_MAX];
    char           rom_dir[POSIX_SS_DIR_MAX];   /* base_dir/<8.3 name> */
    SaveStateEntry entries[SAVE_STATE_STORE_MAX_ENTRIES];
    size_t         entry_count;
} PosixSaveState;

static int compare_entry_desc(const void *a, const void *b) {
    const SaveStateEntry *ea = (const SaveStateEntry *)a;
    const SaveStateEntry *eb = (const SaveStateEntry *)b;
    if (ea->elapsed_seconds == eb->elapsed_seconds) return 0;
    return (ea->elapsed_seconds > eb->elapsed_seconds) ? -1 : 1;
}

/* Parse "<digits>.SAV" (case-insensitive) into elapsed_seconds.  Returns
 * false if name doesn't match that pattern. */
static bool parse_save_name(const char *name, uint32_t *out_elapsed) {
    size_t n = strlen(name);
    if (n < 5 || n > 12) return false;
    const char *ext = &name[n - 4];
    if (ext[0] != '.' ||
        (ext[1] != 's' && ext[1] != 'S') ||
        (ext[2] != 'a' && ext[2] != 'A') ||
        (ext[3] != 'v' && ext[3] != 'V')) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < n - 4; ++i) {
        if (!isdigit((unsigned char)name[i])) return false;
        value = value * 10u + (uint32_t)(name[i] - '0');
    }
    *out_elapsed = value;
    return true;
}

static size_t posix_ss_refresh(SaveStateStore *self, const char *rom_name) {
    PosixSaveState *st = (PosixSaveState *)self->user;
    st->entry_count = 0;

    char dir_name[9];
    save_state_store_dir_name(rom_name, dir_name);
    snprintf(st->rom_dir, sizeof(st->rom_dir), "%s/%s", st->base_dir, dir_name);

    DIR *dir = opendir(st->rom_dir);
    if (dir == NULL) {
        return 0;
    }

    struct dirent *de;
    while (st->entry_count < SAVE_STATE_STORE_MAX_ENTRIES &&
           (de = readdir(dir)) != NULL) {
        uint32_t elapsed;
        if (!parse_save_name(de->d_name, &elapsed)) continue;

        SaveStateEntry *e = &st->entries[st->entry_count];
        e->elapsed_seconds = elapsed;
        save_state_format_elapsed(elapsed, e->label, sizeof(e->label));
        ++st->entry_count;
    }
    closedir(dir);

    qsort(st->entries, st->entry_count, sizeof(*st->entries), compare_entry_desc);
    return st->entry_count;
}

static size_t posix_ss_count(SaveStateStore *self) {
    PosixSaveState *st = (PosixSaveState *)self->user;
    return st->entry_count;
}

static const SaveStateEntry *posix_ss_entry(SaveStateStore *self, size_t index) {
    PosixSaveState *st = (PosixSaveState *)self->user;
    if (index >= st->entry_count) return NULL;
    return &st->entries[index];
}

static bool posix_ss_load(SaveStateStore *self, size_t index, SaveStateBlob *out) {
    PosixSaveState *st = (PosixSaveState *)self->user;
    if (index >= st->entry_count) return false;

    char file_name[13];
    save_state_store_file_name(st->entries[index].elapsed_seconds, file_name);

    char path[POSIX_SS_DIR_MAX + 14];
    snprintf(path, sizeof(path), "%s/%s", st->rom_dir, file_name);

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return false;
    size_t got = fread(out, 1, sizeof(*out), fp);
    fclose(fp);
    return got == sizeof(*out);
}

static bool posix_ss_save(SaveStateStore *self, SaveStateBlob *blob) {
    PosixSaveState *st = (PosixSaveState *)self->user;
    if (st->rom_dir[0] == '\0') return false;

    /* Best-effort directory creation; EEXIST is fine. */
    mkdir(st->rom_dir, 0755);

    uint32_t elapsed = blob->header.elapsed_seconds;
    uint32_t candidate = elapsed;
    char path[POSIX_SS_DIR_MAX + 14];

    /* Resolve filename collisions (multiple saves within the same second) by
     * trying elapsed, elapsed+1, ... elapsed+59 for a free name.  Falls back
     * to overwriting the last-tried name if all are taken. */
    for (int delta = 0; delta < 60; ++delta) {
        candidate = elapsed + (uint32_t)delta;
        char file_name[13];
        save_state_store_file_name(candidate, file_name);
        snprintf(path, sizeof(path), "%s/%s", st->rom_dir, file_name);

        FILE *probe = fopen(path, "rb");
        if (probe == NULL) {
            break;
        }
        fclose(probe);
    }

    /* If a collision pushed the on-disk filename's elapsed time away from
     * the blob's recorded value, update the blob (and its CRC) to match so
     * the save menu highlights the entry that was actually written. */
    if (candidate != elapsed) {
        blob->header.elapsed_seconds = candidate;
        blob->crc32 = save_state_crc32((const uint8_t *)blob, offsetof(SaveStateBlob, crc32));
    }

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) return false;
    size_t wrote = fwrite(blob, 1, sizeof(*blob), fp);
    fclose(fp);
    if (wrote != sizeof(*blob)) return false;

    /* Re-scan rom_dir (already set by the most recent refresh()) so
     * count()/entry() reflect the newly written file. */
    st->entry_count = 0;
    DIR *dir = opendir(st->rom_dir);
    if (dir != NULL) {
        struct dirent *de;
        while (st->entry_count < SAVE_STATE_STORE_MAX_ENTRIES &&
               (de = readdir(dir)) != NULL) {
            uint32_t e;
            if (!parse_save_name(de->d_name, &e)) continue;
            st->entries[st->entry_count].elapsed_seconds = e;
            save_state_format_elapsed(e, st->entries[st->entry_count].label,
                                      sizeof(st->entries[st->entry_count].label));
            ++st->entry_count;
        }
        closedir(dir);
    }
    qsort(st->entries, st->entry_count, sizeof(*st->entries), compare_entry_desc);
    return true;
}

static bool posix_ss_clear_all(SaveStateStore *self) {
    PosixSaveState *st = (PosixSaveState *)self->user;
    if (st->rom_dir[0] == '\0') return true;

    DIR *dir = opendir(st->rom_dir);
    if (dir == NULL) {
        st->entry_count = 0;
        return true;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        uint32_t elapsed;
        if (!parse_save_name(de->d_name, &elapsed)) continue;

        char path[POSIX_SS_DIR_MAX + 14];
        snprintf(path, sizeof(path), "%s/%s", st->rom_dir, de->d_name);
        remove(path);
    }
    closedir(dir);

    st->entry_count = 0;
    return true;
}

bool save_state_store_posix_init(SaveStateStore *out, const char *base_dir) {
    if (out == NULL || base_dir == NULL) return false;

    PosixSaveState *st = (PosixSaveState *)calloc(1, sizeof(*st));
    if (st == NULL) return false;

    snprintf(st->base_dir, sizeof(st->base_dir), "%.*s",
             (int)sizeof(st->base_dir) - 1, base_dir);
    size_t n = strlen(st->base_dir);
    while (n > 1 && st->base_dir[n - 1] == '/') {
        st->base_dir[--n] = '\0';
    }

    mkdir(st->base_dir, 0755);

    memset(out, 0, sizeof(*out));
    out->user      = st;
    out->refresh   = posix_ss_refresh;
    out->count     = posix_ss_count;
    out->entry     = posix_ss_entry;
    out->load      = posix_ss_load;
    out->save      = posix_ss_save;
    out->clear_all = posix_ss_clear_all;
    return true;
}

void save_state_store_posix_destroy(SaveStateStore *store) {
    if (store == NULL || store->user == NULL) return;
    free(store->user);
    memset(store, 0, sizeof(*store));
}
