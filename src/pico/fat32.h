#ifndef MICRONES_FAT32_H
#define MICRONES_FAT32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal read-only FAT32 reader.  Supports a single primary FAT32
 * partition and reads filenames as LFN (UTF-16LE -> ASCII fallback).
 * Built on top of sd_spi block I/O. */

enum {
    FAT32_NAME_MAX = 255,
};

#define FAT32_ATTR_READ_ONLY 0x01u
#define FAT32_ATTR_HIDDEN    0x02u
#define FAT32_ATTR_SYSTEM    0x04u
#define FAT32_ATTR_VOLUME_ID 0x08u
#define FAT32_ATTR_DIRECTORY 0x10u
#define FAT32_ATTR_ARCHIVE   0x20u
#define FAT32_ATTR_LFN       0x0Fu

typedef struct {
    char     name[FAT32_NAME_MAX + 1];
    uint32_t first_cluster;
    uint32_t size;
    uint8_t  attr;
} Fat32Entry;

typedef struct {
    uint32_t cluster;
    uint32_t sector_in_cluster;  /* 0..sectors_per_cluster-1 */
    uint32_t entry_in_sector;    /* 0..15  (16 entries per 512B sector) */
    uint8_t  sector[512];
    bool     sector_loaded;

    /* LFN reassembly. */
    char     lfn[FAT32_NAME_MAX + 1];
    bool     lfn_pending;
} Fat32DirIter;

typedef struct {
    uint32_t first_cluster;
    uint32_t size;
    uint32_t offset;
    uint32_t cur_cluster;
    uint32_t cur_byte_in_cluster;  /* bytes consumed from cur_cluster */
} Fat32File;

bool fat32_mount(void);
bool fat32_is_mounted(void);

/* Open the root directory for iteration. */
bool fat32_open_root(Fat32DirIter *it);

/* Open a subdirectory for iteration, given its first-cluster number (which
 * comes from a Fat32Entry that has FAT32_ATTR_DIRECTORY set). */
bool fat32_open_dir_at_cluster(Fat32DirIter *it, uint32_t first_cluster);

/* Fetch the next non-deleted, non-volume-id, non-LFN entry.  Returns false
 * at end-of-directory or on read error. */
bool fat32_dir_next(Fat32DirIter *it, Fat32Entry *out);

/* Open a file from a directory entry. */
void fat32_open_file(Fat32File *f, const Fat32Entry *e);

/* Read up to `size` bytes; returns the number actually read.  Reads less
 * than requested when EOF or on error. */
size_t fat32_read(Fat32File *f, uint8_t *buf, size_t size);

/* --- Write support ---
 *
 * All of the functions below operate on 8.3 short names only (e.g.
 * "SUPERMAR" or "00001234.SAV") and never create or read LFN entries.
 * `dir_cluster == 0` means the root directory. */

/* Find a subdirectory of dir_cluster by 8.3 name (case-insensitive).
 * Returns its first cluster, or 0 if not found. */
uint32_t fat32_find_dir(uint32_t dir_cluster, const char *name83);

/* Find or create a subdirectory of dir_cluster with the given 8.3 name.
 * Returns its first cluster, or 0 on error. */
uint32_t fat32_find_or_create_dir(uint32_t dir_cluster, const char *name83);

/* Find a file by 8.3 name (case-insensitive) within dir_cluster.  On
 * success fills *out and returns true. */
bool fat32_find_file(uint32_t dir_cluster, const char *name83, Fat32Entry *out);

/* Create or overwrite a file with the given 8.3 name inside dir_cluster,
 * writing `size` bytes from `data`.  Returns true on success. */
bool fat32_write_file(uint32_t dir_cluster, const char *name83,
                      const uint8_t *data, uint32_t size);

/* Delete a file by 8.3 name inside dir_cluster, freeing its cluster chain.
 * Returns true if deleted or if it did not exist. */
bool fat32_delete_file(uint32_t dir_cluster, const char *name83);

#endif
