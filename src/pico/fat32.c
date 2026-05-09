#include "fat32.h"

#include "sd_spi.h"

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
