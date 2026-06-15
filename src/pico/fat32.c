#include "fat32.h"

#include "sd_spi.h"

#include <ctype.h>
#include <string.h>

typedef struct {
    bool     mounted;
    uint32_t partition_lba;
    uint32_t fat_lba;
    uint32_t data_lba;
    uint32_t root_cluster;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t sectors_per_fat;

    /* One-sector FAT cache.  Whole FAT can be megabytes; per-cluster reads
     * would otherwise re-fetch the same sector repeatedly when reading a
     * sequential file.  Trades 512 B for an order of magnitude fewer SD
     * transactions during ROM loads. */
    uint32_t fat_cached_sector;
    uint8_t  fat_cache[512];
    bool     fat_cache_valid;

    /* Write support: total data clusters (for free-cluster scans) and a
     * "search from here" hint to avoid rescanning from cluster 2 every
     * time. */
    uint32_t total_clusters;
    uint32_t next_free_hint;
} Fat32State;

static Fat32State s_fs;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool fat32_is_mounted(void) {
    return s_fs.mounted;
}

static bool read_sector(uint32_t lba, uint8_t *buf) {
    return sd_read_block(lba, buf) == SD_OK;
}

static uint32_t cluster_first_lba(uint32_t cluster) {
    return s_fs.data_lba + (cluster - 2u) * s_fs.sectors_per_cluster;
}

static uint32_t fat_entry(uint32_t cluster) {
    /* FAT32: 4 bytes per entry, mask off top 4 bits. */
    uint32_t fat_offset = cluster * 4u;
    uint32_t sector = s_fs.fat_lba + (fat_offset / s_fs.bytes_per_sector);
    uint32_t entry_offset = fat_offset % s_fs.bytes_per_sector;

    if (!s_fs.fat_cache_valid || s_fs.fat_cached_sector != sector) {
        if (!read_sector(sector, s_fs.fat_cache)) {
            s_fs.fat_cache_valid = false;
            return 0x0FFFFFFFu;  /* treat as EOF on error */
        }
        s_fs.fat_cached_sector = sector;
        s_fs.fat_cache_valid = true;
    }
    return rd32(&s_fs.fat_cache[entry_offset]) & 0x0FFFFFFFu;
}

static bool cluster_is_eof(uint32_t cluster) {
    return cluster >= 0x0FFFFFF8u || cluster < 2u;
}

static bool parse_bpb(const uint8_t *bpb, uint32_t partition_lba) {
    s_fs.bytes_per_sector    = rd16(&bpb[11]);
    s_fs.sectors_per_cluster = bpb[13];
    s_fs.reserved_sectors    = rd16(&bpb[14]);
    s_fs.num_fats            = bpb[16];
    uint16_t fat16_size      = rd16(&bpb[22]);  /* 0 for FAT32 */
    uint32_t fat32_size      = rd32(&bpb[36]);
    s_fs.sectors_per_fat     = (fat16_size != 0u) ? fat16_size : fat32_size;
    s_fs.root_cluster        = rd32(&bpb[44]);

    if (s_fs.bytes_per_sector != 512u) return false;
    if (s_fs.sectors_per_cluster == 0u) return false;
    if (s_fs.num_fats == 0u) return false;
    if (s_fs.sectors_per_fat == 0u) return false;
    if (s_fs.root_cluster < 2u) return false;

    s_fs.partition_lba = partition_lba;
    s_fs.fat_lba       = partition_lba + s_fs.reserved_sectors;
    s_fs.data_lba      = s_fs.fat_lba + (uint32_t)s_fs.num_fats * s_fs.sectors_per_fat;

    uint32_t total_sectors = rd32(&bpb[32]);
    uint32_t reserved_for_fats = s_fs.reserved_sectors + (uint32_t)s_fs.num_fats * s_fs.sectors_per_fat;
    uint32_t data_sectors = (total_sectors > reserved_for_fats) ? total_sectors - reserved_for_fats : 0u;
    s_fs.total_clusters = data_sectors / s_fs.sectors_per_cluster;
    s_fs.next_free_hint = 2u;
    return true;
}

bool fat32_mount(void) {
    memset(&s_fs, 0, sizeof(s_fs));

    uint8_t sec[512];
    if (!read_sector(0, sec)) {
        return false;
    }

    /* Boot signature check at offset 510. */
    if (sec[510] != 0x55u || sec[511] != 0xAAu) {
        return false;
    }

    /* Two layouts to handle: a real MBR with a partition table, or a
     * superfloppy where the BPB lives at LBA 0.  Detect a BPB by the bytes-
     * per-sector field (offset 11) being a sane power-of-two and the
     * jump opcode (offset 0) being a valid x86 boot prologue (0xEB or 0xE9). */
    uint8_t jmp = sec[0];
    if ((jmp == 0xEBu || jmp == 0xE9u) && rd16(&sec[11]) == 512u) {
        if (parse_bpb(sec, 0)) {
            s_fs.mounted = true;
            return true;
        }
    }

    /* Real MBR: scan the four primary partition entries at offset 446. */
    for (int i = 0; i < 4; ++i) {
        const uint8_t *part = &sec[446 + i * 16];
        uint8_t type = part[4];
        uint32_t lba = rd32(&part[8]);
        if (type == 0x0Bu || type == 0x0Cu || type == 0x0Eu) {
            uint8_t bpb[512];
            if (!read_sector(lba, bpb)) continue;
            if (parse_bpb(bpb, lba)) {
                s_fs.mounted = true;
                return true;
            }
        }
    }

    return false;
}

static void load_dir_sector(Fat32DirIter *it) {
    uint32_t lba = cluster_first_lba(it->cluster) + it->sector_in_cluster;
    it->sector_loaded = read_sector(lba, it->sector);
}

bool fat32_open_root(Fat32DirIter *it) {
    if (!s_fs.mounted) return false;
    memset(it, 0, sizeof(*it));
    it->cluster = s_fs.root_cluster;
    return true;
}

bool fat32_open_dir_at_cluster(Fat32DirIter *it, uint32_t first_cluster) {
    if (!s_fs.mounted) return false;
    if (first_cluster < 2u || cluster_is_eof(first_cluster)) return false;
    memset(it, 0, sizeof(*it));
    it->cluster = first_cluster;
    return true;
}

/* Convert a 13-char LFN slot into ASCII characters in *buf (advancing
 * *cursor).  Stops at the NUL/0xFFFF terminator. */
static void lfn_extract_chunk(const uint8_t *entry, char *out, size_t *cursor) {
    static const uint8_t offsets[13] = {
        1, 3, 5, 7, 9,
        14, 16, 18, 20, 22, 24,
        28, 30,
    };
    for (int i = 0; i < 13; ++i) {
        uint16_t ch = (uint16_t)(entry[offsets[i]] | (entry[offsets[i] + 1] << 8));
        if (ch == 0u || ch == 0xFFFFu) {
            return;
        }
        char c = (ch >= 0x20u && ch < 0x7Fu) ? (char)ch : '?';
        if (*cursor < FAT32_NAME_MAX) {
            out[(*cursor)++] = c;
        }
    }
}

static void format_short_name(const uint8_t *entry, char *out) {
    /* Bytes 0..7 = name (space-padded), 8..10 = ext.  First byte 0x05 maps
     * back to 0xE5 (Japanese filenames). */
    uint8_t name[11];
    memcpy(name, entry, 11);
    if (name[0] == 0x05u) name[0] = 0xE5u;

    size_t out_cursor = 0;
    for (int i = 0; i < 8 && name[i] != ' '; ++i) {
        out[out_cursor++] = (char)name[i];
    }
    if (name[8] != ' ') {
        out[out_cursor++] = '.';
        for (int i = 8; i < 11 && name[i] != ' '; ++i) {
            out[out_cursor++] = (char)name[i];
        }
    }
    out[out_cursor] = '\0';
}

static bool advance_iter(Fat32DirIter *it) {
    ++it->entry_in_sector;
    if (it->entry_in_sector < 16u) {
        return true;
    }
    it->entry_in_sector = 0;
    ++it->sector_in_cluster;
    if (it->sector_in_cluster < s_fs.sectors_per_cluster) {
        it->sector_loaded = false;
        return true;
    }
    it->sector_in_cluster = 0;
    uint32_t next = fat_entry(it->cluster);
    if (cluster_is_eof(next)) {
        return false;
    }
    it->cluster = next;
    it->sector_loaded = false;
    return true;
}

bool fat32_dir_next(Fat32DirIter *it, Fat32Entry *out) {
    if (!s_fs.mounted) return false;

    while (true) {
        if (!it->sector_loaded) {
            load_dir_sector(it);
            if (!it->sector_loaded) return false;
        }
        const uint8_t *entry = &it->sector[it->entry_in_sector * 32u];

        /* End-of-directory marker. */
        if (entry[0] == 0x00u) {
            return false;
        }

        /* Deleted entry. */
        if (entry[0] == 0xE5u) {
            it->lfn_pending = false;
            it->lfn[0] = '\0';
            if (!advance_iter(it)) return false;
            continue;
        }

        uint8_t attr = entry[11];

        if (attr == FAT32_ATTR_LFN) {
            /* LFN entries arrive in reverse order: the first physical entry
             * has the highest sequence number with bit 6 set ("last_long").
             * We rebuild the name by writing each chunk into the position
             * given by (seq-1)*13. */
            uint8_t order = (uint8_t)(entry[0] & 0x1Fu);
            if (order >= 1u && order <= 20u) {
                char chunk[14] = { 0 };
                size_t cursor = 0;
                lfn_extract_chunk(entry, chunk, &cursor);
                size_t base = (size_t)(order - 1u) * 13u;
                for (size_t i = 0; i < cursor && base + i < FAT32_NAME_MAX; ++i) {
                    it->lfn[base + i] = chunk[i];
                }
                if ((entry[0] & 0x40u) != 0u) {
                    /* "Last" LFN entry seen first — it knows the length.
                     * Pad behind with NULs so we don't trail garbage. */
                    size_t end = base + cursor;
                    if (end < sizeof(it->lfn)) {
                        it->lfn[end] = '\0';
                    }
                }
                it->lfn_pending = true;
            }
            if (!advance_iter(it)) return false;
            continue;
        }

        /* Skip volume-id entries, but expose hidden/system files (some
         * users put ROMs in hidden folders). */
        if ((attr & FAT32_ATTR_VOLUME_ID) != 0u) {
            it->lfn_pending = false;
            it->lfn[0] = '\0';
            if (!advance_iter(it)) return false;
            continue;
        }

        /* Build the entry. */
        memset(out, 0, sizeof(*out));
        out->attr = attr;
        out->size = rd32(&entry[28]);
        uint32_t cluster_hi = rd16(&entry[20]);
        uint32_t cluster_lo = rd16(&entry[26]);
        out->first_cluster = (cluster_hi << 16) | cluster_lo;

        if (it->lfn_pending && it->lfn[0] != '\0') {
            size_t lfn_len = strnlen(it->lfn, FAT32_NAME_MAX);
            memcpy(out->name, it->lfn, lfn_len);
            out->name[lfn_len] = '\0';
        } else {
            format_short_name(entry, out->name);
        }
        it->lfn_pending = false;
        it->lfn[0] = '\0';

        /* Advance for the next call. */
        if (!advance_iter(it)) {
            /* That was the last entry but we still want to return it. */
            it->cluster = 0;  /* mark exhausted */
        }
        return true;
    }
}

void fat32_open_file(Fat32File *f, const Fat32Entry *e) {
    memset(f, 0, sizeof(*f));
    f->first_cluster = e->first_cluster;
    f->cur_cluster   = e->first_cluster;
    f->size          = e->size;
}

size_t fat32_read(Fat32File *f, uint8_t *buf, size_t size) {
    if (!s_fs.mounted || f->size == 0) return 0;
    if (f->offset >= f->size) return 0;

    size_t want = size;
    if (f->offset + want > f->size) {
        want = f->size - f->offset;
    }

    size_t cluster_bytes = (size_t)s_fs.sectors_per_cluster * 512u;
    size_t total_read = 0;

    uint8_t scratch[512];

    while (want > 0 && !cluster_is_eof(f->cur_cluster)) {
        if (f->cur_byte_in_cluster >= cluster_bytes) {
            /* Roll over to next cluster. */
            uint32_t next = fat_entry(f->cur_cluster);
            if (cluster_is_eof(next)) break;
            f->cur_cluster = next;
            f->cur_byte_in_cluster = 0;
        }

        uint32_t sector_in_cluster = f->cur_byte_in_cluster / 512u;
        uint32_t byte_in_sector    = f->cur_byte_in_cluster % 512u;
        uint32_t lba = cluster_first_lba(f->cur_cluster) + sector_in_cluster;

        /* Fast path: sector-aligned, full-sector read straight into caller. */
        if (byte_in_sector == 0u && want >= 512u) {
            if (!read_sector(lba, buf)) break;
            buf += 512u;
            want -= 512u;
            total_read += 512u;
            f->offset += 512u;
            f->cur_byte_in_cluster += 512u;
            continue;
        }

        /* Slow path: partial sector. */
        if (!read_sector(lba, scratch)) break;
        size_t copy = 512u - byte_in_sector;
        if (copy > want) copy = want;
        memcpy(buf, &scratch[byte_in_sector], copy);
        buf += copy;
        want -= copy;
        total_read += copy;
        f->offset += copy;
        f->cur_byte_in_cluster += copy;
    }

    return total_read;
}

/* ------------------------------------------------------------------ */
/* Write support                                                        */
/* ------------------------------------------------------------------ */

static bool write_sector(uint32_t lba, const uint8_t *buf) {
    return sd_write_block(lba, buf) == SD_OK;
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Write a FAT32 entry (low 28 bits of `value`, high 4 bits of the existing
 * entry preserved), mirrored across all `num_fats` copies.  Keeps the
 * one-sector FAT read cache coherent. */
static bool fat_set_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset     = cluster * 4u;
    uint32_t sector_in_fat  = fat_offset / s_fs.bytes_per_sector;
    uint32_t entry_offset   = fat_offset % s_fs.bytes_per_sector;
    uint32_t primary_sector = s_fs.fat_lba + sector_in_fat;

    uint8_t sec[512];
    if (s_fs.fat_cache_valid && s_fs.fat_cached_sector == primary_sector) {
        memcpy(sec, s_fs.fat_cache, sizeof(sec));
    } else {
        if (!read_sector(primary_sector, sec)) return false;
    }

    uint32_t old = rd32(&sec[entry_offset]);
    uint32_t updated = (old & 0xF0000000u) | (value & 0x0FFFFFFFu);
    wr32(&sec[entry_offset], updated);

    for (uint8_t i = 0; i < s_fs.num_fats; ++i) {
        uint32_t sector = s_fs.fat_lba + (uint32_t)i * s_fs.sectors_per_fat + sector_in_fat;
        if (!write_sector(sector, sec)) return false;
    }

    s_fs.fat_cached_sector = primary_sector;
    memcpy(s_fs.fat_cache, sec, sizeof(sec));
    s_fs.fat_cache_valid = true;
    return true;
}

/* Zero every sector of `cluster`'s data. */
static bool zero_cluster(uint32_t cluster) {
    uint8_t zero[512] = { 0 };
    uint32_t lba = cluster_first_lba(cluster);
    for (uint8_t i = 0; i < s_fs.sectors_per_cluster; ++i) {
        if (!write_sector(lba + i, zero)) return false;
    }
    return true;
}

/* Find a free (unallocated, FAT entry == 0) cluster, searching from
 * next_free_hint and wrapping around.  Returns 0 if the volume is full. */
static uint32_t find_free_cluster(void) {
    uint32_t start = (s_fs.next_free_hint >= 2u) ? s_fs.next_free_hint : 2u;
    uint32_t limit = s_fs.total_clusters + 2u;

    for (uint32_t c = start; c < limit; ++c) {
        if (fat_entry(c) == 0u) {
            s_fs.next_free_hint = c + 1u;
            return c;
        }
    }
    for (uint32_t c = 2u; c < start; ++c) {
        if (fat_entry(c) == 0u) {
            s_fs.next_free_hint = c + 1u;
            return c;
        }
    }
    return 0u;
}

/* Free every cluster in the chain starting at `first` (no-op if first < 2
 * or already EOF). */
static bool free_cluster_chain(uint32_t first) {
    uint32_t c = first;
    bool first_iter = true;
    while (c >= 2u && !cluster_is_eof(c)) {
        uint32_t next = fat_entry(c);
        if (!fat_set_entry(c, 0u)) return false;
        if (first_iter) {
            if (c < s_fs.next_free_hint) s_fs.next_free_hint = c;
            first_iter = false;
        }
        if (cluster_is_eof(next)) break;
        c = next;
    }
    return true;
}

/* Allocate a chain of `count` freshly-zeroed clusters, terminated with an
 * EOF marker.  On failure, frees any clusters already allocated for this
 * chain and returns false. */
static bool alloc_cluster_chain(uint32_t count, uint32_t *out_first) {
    uint32_t first = 0u, prev = 0u;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t c = find_free_cluster();
        if (c == 0u || !zero_cluster(c) || !fat_set_entry(c, 0x0FFFFFFFu)) {
            if (first != 0u) free_cluster_chain(first);
            return false;
        }
        if (prev != 0u) {
            if (!fat_set_entry(prev, c)) {
                free_cluster_chain(first);
                return false;
            }
        } else {
            first = c;
        }
        prev = c;
    }
    *out_first = first;
    return true;
}

/* Convert a name like "SUPERMAR" or "00001234.SAV" into the fixed 11-byte
 * (8.3, space-padded, uppercase) on-disk form. */
static void to_fat_name(const char *name, uint8_t out[11]) {
    memset(out, ' ', 11);

    const char *dot = strchr(name, '.');
    size_t name_len = dot != NULL ? (size_t)(dot - name) : strlen(name);
    if (name_len > 8u) name_len = 8u;
    for (size_t i = 0; i < name_len; ++i) {
        out[i] = (uint8_t)toupper((unsigned char)name[i]);
    }

    if (dot != NULL) {
        const char *ext = dot + 1;
        size_t ext_len = strlen(ext);
        if (ext_len > 3u) ext_len = 3u;
        for (size_t i = 0; i < ext_len; ++i) {
            out[8u + i] = (uint8_t)toupper((unsigned char)ext[i]);
        }
    }
}

static void build_dir_entry(uint8_t entry[32], const uint8_t name83[11], uint8_t attr,
                            uint32_t first_cluster, uint32_t size) {
    memset(entry, 0, 32);
    memcpy(entry, name83, 11);
    entry[11] = attr;
    wr16(&entry[20], (uint16_t)(first_cluster >> 16));
    wr16(&entry[26], (uint16_t)(first_cluster & 0xFFFFu));
    wr32(&entry[28], size);
}

typedef struct {
    uint32_t lba;
    uint32_t offset; /* byte offset of the 32-byte entry within sector `lba` */
} DirSlot;

/* Advance (cluster, sector_in_cluster) to the next sector in a cluster
 * chain.  Returns false at end-of-chain. */
typedef struct {
    uint32_t cluster;
    uint32_t sector_in_cluster;
} ChainPos;

static bool chain_pos_advance(ChainPos *pos) {
    ++pos->sector_in_cluster;
    if (pos->sector_in_cluster < s_fs.sectors_per_cluster) {
        return true;
    }
    pos->sector_in_cluster = 0u;
    uint32_t next = fat_entry(pos->cluster);
    if (cluster_is_eof(next)) return false;
    pos->cluster = next;
    return true;
}

/* Search dir_cluster (0 = root) for an entry whose raw 11-byte name matches
 * name83.  Skips free/deleted/LFN/volume-id entries.  On success, fills
 * *out_slot (if non-NULL) with its on-disk location and *out_entry (if
 * non-NULL) with its parsed contents. */
static bool dir_find_entry_slot(uint32_t dir_cluster, const uint8_t name83[11],
                                DirSlot *out_slot, Fat32Entry *out_entry) {
    ChainPos pos = { (dir_cluster == 0u) ? s_fs.root_cluster : dir_cluster, 0u };
    uint8_t sec[512];

    for (;;) {
        uint32_t lba = cluster_first_lba(pos.cluster) + pos.sector_in_cluster;
        if (!read_sector(lba, sec)) return false;

        for (int i = 0; i < 16; ++i) {
            const uint8_t *e = &sec[i * 32];
            if (e[0] == 0x00u) return false; /* end of directory */
            if (e[0] == 0xE5u) continue;
            if (e[11] == FAT32_ATTR_LFN) continue;
            if ((e[11] & FAT32_ATTR_VOLUME_ID) != 0u) continue;

            if (memcmp(e, name83, 11) == 0) {
                if (out_slot != NULL) {
                    out_slot->lba    = lba;
                    out_slot->offset = (uint32_t)i * 32u;
                }
                if (out_entry != NULL) {
                    memset(out_entry, 0, sizeof(*out_entry));
                    out_entry->attr = e[11];
                    out_entry->size = rd32(&e[28]);
                    uint32_t hi = rd16(&e[20]);
                    uint32_t lo = rd16(&e[26]);
                    out_entry->first_cluster = (hi << 16) | lo;
                    format_short_name(e, out_entry->name);
                }
                return true;
            }
        }

        if (!chain_pos_advance(&pos)) return false;
    }
}

/* Find the first free (0x00 or 0xE5) directory entry slot in dir_cluster
 * (0 = root), extending the directory's cluster chain with a fresh zeroed
 * cluster if it is full.  Returns false only on allocation/IO failure. */
static bool dir_find_free_slot(uint32_t dir_cluster, DirSlot *out) {
    ChainPos pos = { (dir_cluster == 0u) ? s_fs.root_cluster : dir_cluster, 0u };
    uint8_t sec[512];

    for (;;) {
        uint32_t lba = cluster_first_lba(pos.cluster) + pos.sector_in_cluster;
        if (!read_sector(lba, sec)) return false;

        for (int i = 0; i < 16; ++i) {
            uint8_t b = sec[i * 32];
            if (b == 0x00u || b == 0xE5u) {
                out->lba    = lba;
                out->offset = (uint32_t)i * 32u;
                return true;
            }
        }

        if (!chain_pos_advance(&pos)) {
            uint32_t new_cluster;
            if (!alloc_cluster_chain(1u, &new_cluster)) return false;
            if (!fat_set_entry(pos.cluster, new_cluster)) return false;

            out->lba    = cluster_first_lba(new_cluster);
            out->offset = 0u;
            return true;
        }
    }
}

static bool write_dir_slot(const DirSlot *slot, const uint8_t entry[32]) {
    uint8_t sec[512];
    if (!read_sector(slot->lba, sec)) return false;
    memcpy(&sec[slot->offset], entry, 32);
    return write_sector(slot->lba, sec);
}

uint32_t fat32_find_dir(uint32_t dir_cluster, const char *name83) {
    if (!s_fs.mounted) return 0u;

    uint8_t raw[11];
    to_fat_name(name83, raw);

    Fat32Entry e;
    if (dir_find_entry_slot(dir_cluster, raw, NULL, &e) &&
        (e.attr & FAT32_ATTR_DIRECTORY) != 0u) {
        return e.first_cluster;
    }
    return 0u;
}

uint32_t fat32_find_or_create_dir(uint32_t dir_cluster, const char *name83) {
    if (!s_fs.mounted) return 0u;

    uint8_t raw[11];
    to_fat_name(name83, raw);

    Fat32Entry existing;
    if (dir_find_entry_slot(dir_cluster, raw, NULL, &existing)) {
        return (existing.attr & FAT32_ATTR_DIRECTORY) != 0u ? existing.first_cluster : 0u;
    }

    uint32_t new_cluster;
    if (!alloc_cluster_chain(1u, &new_cluster)) return 0u;

    /* "." and ".." entries.  ".." points at cluster 0 when the parent is
     * the root directory, per the FAT32 convention. */
    uint8_t dot[11], dotdot[11];
    memset(dot, ' ', sizeof(dot));
    memset(dotdot, ' ', sizeof(dotdot));
    dot[0] = '.';
    dotdot[0] = '.';
    dotdot[1] = '.';

    uint32_t parent_for_dotdot =
        (dir_cluster == 0u || dir_cluster == s_fs.root_cluster) ? 0u : dir_cluster;

    uint8_t sec[512];
    memset(sec, 0, sizeof(sec));
    build_dir_entry(&sec[0],  dot,    FAT32_ATTR_DIRECTORY, new_cluster, 0u);
    build_dir_entry(&sec[32], dotdot, FAT32_ATTR_DIRECTORY, parent_for_dotdot, 0u);
    if (!write_sector(cluster_first_lba(new_cluster), sec)) {
        free_cluster_chain(new_cluster);
        return 0u;
    }

    DirSlot slot;
    if (!dir_find_free_slot(dir_cluster, &slot)) {
        free_cluster_chain(new_cluster);
        return 0u;
    }

    uint8_t entry[32];
    build_dir_entry(entry, raw, FAT32_ATTR_DIRECTORY, new_cluster, 0u);
    if (!write_dir_slot(&slot, entry)) {
        free_cluster_chain(new_cluster);
        return 0u;
    }

    return new_cluster;
}

bool fat32_find_file(uint32_t dir_cluster, const char *name83, Fat32Entry *out) {
    if (!s_fs.mounted) return false;

    uint8_t raw[11];
    to_fat_name(name83, raw);

    Fat32Entry e;
    if (dir_find_entry_slot(dir_cluster, raw, NULL, &e) &&
        (e.attr & FAT32_ATTR_DIRECTORY) == 0u) {
        if (out != NULL) *out = e;
        return true;
    }
    return false;
}

bool fat32_write_file(uint32_t dir_cluster, const char *name83,
                      const uint8_t *data, uint32_t size) {
    if (!s_fs.mounted) return false;

    uint8_t raw[11];
    to_fat_name(name83, raw);

    uint32_t cluster_bytes = (uint32_t)s_fs.sectors_per_cluster * 512u;
    uint32_t clusters_needed = (size + cluster_bytes - 1u) / cluster_bytes;
    if (clusters_needed == 0u) clusters_needed = 1u;

    Fat32Entry existing;
    DirSlot slot;
    bool exists = dir_find_entry_slot(dir_cluster, raw, &slot, &existing);
    if (exists && (existing.attr & FAT32_ATTR_DIRECTORY) != 0u) {
        return false; /* name collides with a directory */
    }

    if (exists && existing.first_cluster >= 2u) {
        if (!free_cluster_chain(existing.first_cluster)) return false;
    }

    uint32_t first_cluster;
    if (!alloc_cluster_chain(clusters_needed, &first_cluster)) return false;

    uint32_t cluster  = first_cluster;
    uint32_t written  = 0u;
    uint8_t  buf[512];
    while (written < size) {
        for (uint8_t s = 0; s < s_fs.sectors_per_cluster && written < size; ++s) {
            uint32_t chunk = size - written;
            uint32_t lba = cluster_first_lba(cluster) + s;
            if (chunk >= 512u) {
                if (!write_sector(lba, data + written)) {
                    free_cluster_chain(first_cluster);
                    return false;
                }
                written += 512u;
            } else {
                memset(buf, 0, sizeof(buf));
                memcpy(buf, data + written, chunk);
                if (!write_sector(lba, buf)) {
                    free_cluster_chain(first_cluster);
                    return false;
                }
                written += chunk;
            }
        }
        if (written < size) {
            cluster = fat_entry(cluster);
            if (cluster_is_eof(cluster)) break;
        }
    }

    uint8_t entry[32];
    if (exists) {
        build_dir_entry(entry, raw, existing.attr, first_cluster, size);
        return write_dir_slot(&slot, entry);
    }

    if (!dir_find_free_slot(dir_cluster, &slot)) {
        free_cluster_chain(first_cluster);
        return false;
    }
    build_dir_entry(entry, raw, FAT32_ATTR_ARCHIVE, first_cluster, size);
    return write_dir_slot(&slot, entry);
}

bool fat32_delete_file(uint32_t dir_cluster, const char *name83) {
    if (!s_fs.mounted) return false;

    uint8_t raw[11];
    to_fat_name(name83, raw);

    Fat32Entry existing;
    DirSlot slot;
    if (!dir_find_entry_slot(dir_cluster, raw, &slot, &existing)) {
        return true; /* nothing to delete */
    }
    if ((existing.attr & FAT32_ATTR_DIRECTORY) != 0u) {
        return false; /* refuse to delete directories */
    }

    if (existing.first_cluster >= 2u) {
        if (!free_cluster_chain(existing.first_cluster)) return false;
    }

    uint8_t sec[512];
    if (!read_sector(slot.lba, sec)) return false;
    sec[slot.offset] = 0xE5u;
    return write_sector(slot.lba, sec);
}
