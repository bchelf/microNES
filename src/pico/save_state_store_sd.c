#include "save_state_store_sd.h"

#include "fat32.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char           rom_dir_name[9];  /* 8.3 dir name for the most recent refresh() */
    uint32_t       rom_dir_cluster;  /* 0 if the ROM's save directory doesn't exist yet */
    SaveStateEntry entries[SAVE_STATE_STORE_MAX_ENTRIES];
    size_t         entry_count;
} SdSaveState;

static SdSaveState s_sd_save_state;

static const char SAVES_ROOT_DIR[] = "SAVES";

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

/* Re-scan st->rom_dir_cluster for "<digits>.SAV" entries into st->entries,
 * sorted newest-first.  Leaves entry_count == 0 if rom_dir_cluster == 0. */
static void rescan_entries(SdSaveState *st) {
    st->entry_count = 0;
    if (st->rom_dir_cluster == 0u) return;

    Fat32DirIter it;
    if (!fat32_open_dir_at_cluster(&it, st->rom_dir_cluster)) return;

    Fat32Entry e;
    while (st->entry_count < SAVE_STATE_STORE_MAX_ENTRIES && fat32_dir_next(&it, &e)) {
        if ((e.attr & FAT32_ATTR_DIRECTORY) != 0u) continue;

        uint32_t elapsed;
        if (!parse_save_name(e.name, &elapsed)) continue;

        SaveStateEntry *entry = &st->entries[st->entry_count];
        entry->elapsed_seconds = elapsed;
        save_state_format_elapsed(elapsed, entry->label, sizeof(entry->label));
        ++st->entry_count;
    }

    qsort(st->entries, st->entry_count, sizeof(*st->entries), compare_entry_desc);
}

static size_t sd_ss_refresh(SaveStateStore *self, const char *rom_name) {
    SdSaveState *st = (SdSaveState *)self->user;

    save_state_store_dir_name(rom_name, st->rom_dir_name);

    uint32_t saves_root = fat32_find_dir(0u, SAVES_ROOT_DIR);
    st->rom_dir_cluster = (saves_root != 0u)
        ? fat32_find_dir(saves_root, st->rom_dir_name)
        : 0u;

    rescan_entries(st);
    return st->entry_count;
}

static size_t sd_ss_count(SaveStateStore *self) {
    SdSaveState *st = (SdSaveState *)self->user;
    return st->entry_count;
}

static const SaveStateEntry *sd_ss_entry(SaveStateStore *self, size_t index) {
    SdSaveState *st = (SdSaveState *)self->user;
    if (index >= st->entry_count) return NULL;
    return &st->entries[index];
}

static bool sd_ss_load(SaveStateStore *self, size_t index, SaveStateBlob *out) {
    SdSaveState *st = (SdSaveState *)self->user;
    if (index >= st->entry_count || st->rom_dir_cluster == 0u) return false;

    char file_name[13];
    save_state_store_file_name(st->entries[index].elapsed_seconds, file_name);

    Fat32Entry fe;
    if (!fat32_find_file(st->rom_dir_cluster, file_name, &fe)) return false;
    if (fe.size != sizeof(*out)) return false;

    Fat32File f;
    fat32_open_file(&f, &fe);
    return fat32_read(&f, (uint8_t *)out, sizeof(*out)) == sizeof(*out);
}

static bool sd_ss_save(SaveStateStore *self, SaveStateBlob *blob) {
    SdSaveState *st = (SdSaveState *)self->user;

    uint32_t saves_root = fat32_find_or_create_dir(0u, SAVES_ROOT_DIR);
    if (saves_root == 0u) return false;

    if (st->rom_dir_cluster == 0u) {
        st->rom_dir_cluster = fat32_find_or_create_dir(saves_root, st->rom_dir_name);
        if (st->rom_dir_cluster == 0u) return false;
    }

    uint32_t elapsed = blob->header.elapsed_seconds;
    uint32_t candidate = elapsed;
    char file_name[13];

    /* Resolve filename collisions the same way as the host store: try
     * elapsed, elapsed+1, ... elapsed+59 for a name not already listed.
     * Falls back to overwriting the last-tried name if all are taken. */
    for (int delta = 0; delta < 60; ++delta) {
        candidate = elapsed + (uint32_t)delta;
        save_state_store_file_name(candidate, file_name);

        bool taken = false;
        for (size_t i = 0; i < st->entry_count; ++i) {
            if (st->entries[i].elapsed_seconds == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) break;
    }

    /* If a collision pushed the on-disk filename's elapsed time away from
     * the blob's recorded value, update the blob (and its CRC) to match so
     * the save menu highlights the entry that was actually written. */
    if (candidate != elapsed) {
        blob->header.elapsed_seconds = candidate;
        blob->crc32 = save_state_crc32((const uint8_t *)blob, offsetof(SaveStateBlob, crc32));
    }

    if (!fat32_write_file(st->rom_dir_cluster, file_name,
                          (const uint8_t *)blob, (uint32_t)sizeof(*blob))) {
        return false;
    }

    rescan_entries(st);
    return true;
}

static bool sd_ss_delete(SaveStateStore *self, size_t index) {
    SdSaveState *st = (SdSaveState *)self->user;
    if (index >= st->entry_count || st->rom_dir_cluster == 0u) return false;

    char file_name[13];
    save_state_store_file_name(st->entries[index].elapsed_seconds, file_name);

    if (!fat32_delete_file(st->rom_dir_cluster, file_name)) return false;

    rescan_entries(st);
    return true;
}

static bool sd_ss_clear_all(SaveStateStore *self) {
    SdSaveState *st = (SdSaveState *)self->user;
    if (st->rom_dir_cluster == 0u) {
        st->entry_count = 0;
        return true;
    }

    for (size_t i = 0; i < st->entry_count; ++i) {
        char file_name[13];
        save_state_store_file_name(st->entries[i].elapsed_seconds, file_name);
        if (!fat32_delete_file(st->rom_dir_cluster, file_name)) {
            return false;
        }
    }

    st->entry_count = 0;
    return true;
}

bool save_state_store_sd_init(SaveStateStore *out) {
    if (out == NULL) return false;

    memset(&s_sd_save_state, 0, sizeof(s_sd_save_state));

    memset(out, 0, sizeof(*out));
    out->user      = &s_sd_save_state;
    out->refresh   = sd_ss_refresh;
    out->count     = sd_ss_count;
    out->entry     = sd_ss_entry;
    out->load      = sd_ss_load;
    out->save      = sd_ss_save;
    out->clear_all = sd_ss_clear_all;
    out->delete_entry = sd_ss_delete;
    return true;
}
