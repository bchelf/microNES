#include "rom_source_flash_fs.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"

#include "pico_video_backend.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef MICRONES_PICO_FLASH_CACHE_OFFSET
#define MICRONES_PICO_FLASH_CACHE_OFFSET 0
#endif

#ifndef MICRONES_PICO_FLASH_CACHE_SIZE
#define MICRONES_PICO_FLASH_CACHE_SIZE 0
#endif

#define FLASH_FS_MAGIC       "MNES"
#define FLASH_FS_VERSION     1
#define FLASH_FS_DIR_SECTORS 2
#define FLASH_FS_DIR_SIZE    (FLASH_FS_DIR_SECTORS * FLASH_SECTOR_SIZE)
#define FLASH_FS_MAX_ROMS    63
#define FLASH_FS_NAME_MAX    96

typedef struct __attribute__((packed)) {
    char     name[FLASH_FS_NAME_MAX];
    uint32_t data_offset;
    uint32_t data_size;
    uint16_t mapper;
    uint8_t  has_battery;
    uint8_t  supported;
    uint32_t prg_size;
    uint32_t chr_size;
    uint8_t  reserved[12];
} FlashFsDirEntry;

_Static_assert(sizeof(FlashFsDirEntry) == 128, "entry must be 128 bytes");

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint8_t  version;
    uint8_t  rom_count;
    uint8_t  reserved[2];
    FlashFsDirEntry entries[FLASH_FS_MAX_ROMS];
} FlashFsHeader;

_Static_assert(sizeof(FlashFsHeader) <= FLASH_FS_DIR_SIZE, "header must fit in dir sectors");

typedef struct {
    RomSourceEntry meta[FLASH_FS_MAX_ROMS];
    size_t         count;
} FlashFsState;

static FlashFsState s_state;
static char s_last_error[160];
static FlashFsProgressFn s_progress_fn;
static void *s_progress_user;

extern char __flash_binary_end;

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
}

static uint32_t align_up_sector(uint32_t v) {
    return (v + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
}

static const FlashFsHeader *xip_header(void) {
    return (const FlashFsHeader *)((uintptr_t)XIP_BASE +
                                   (uintptr_t)MICRONES_PICO_FLASH_CACHE_OFFSET);
}

static bool header_valid(const FlashFsHeader *h) {
    return memcmp(h->magic, FLASH_FS_MAGIC, 4) == 0 &&
           h->version == FLASH_FS_VERSION &&
           h->rom_count <= FLASH_FS_MAX_ROMS;
}

static void load_directory(void) {
    const FlashFsHeader *h = xip_header();
    s_state.count = 0;

    if (!header_valid(h)) return;

    for (uint8_t i = 0; i < h->rom_count && i < FLASH_FS_MAX_ROMS; ++i) {
        const FlashFsDirEntry *de = &h->entries[i];
        RomSourceEntry *m = &s_state.meta[i];
        memset(m, 0, sizeof(*m));
        snprintf(m->name, sizeof(m->name), "%.*s",
                 (int)(sizeof(m->name) - 1), de->name);
        m->mapper      = de->mapper;
        m->supported   = de->supported != 0;
        m->has_battery = de->has_battery != 0;
        m->prg_size    = de->prg_size;
        m->chr_size    = de->chr_size;
        m->file_size   = de->data_size;
        ++s_state.count;
    }
}

static void report_progress(size_t done, size_t total) {
    if (s_progress_fn != NULL)
        s_progress_fn(done, total, s_progress_user);
}

static size_t fs_count(RomSource *self) {
    (void)self;
    return s_state.count;
}

static const RomSourceEntry *fs_entry(RomSource *self, size_t index) {
    (void)self;
    if (index >= s_state.count) return NULL;
    return &s_state.meta[index];
}

static bool fs_load(RomSource *self, size_t index,
                    uint8_t **out_buf, size_t *out_size,
                    char *err, size_t err_size) {
    (void)self;
    if (index >= s_state.count) {
        if (err && err_size) snprintf(err, err_size, "index out of range");
        return false;
    }

    const FlashFsHeader *h = xip_header();
    if (!header_valid(h)) {
        if (err && err_size) snprintf(err, err_size, "flash filesystem corrupt");
        return false;
    }

    const FlashFsDirEntry *de = &h->entries[index];
    const uint8_t *rom_data = (const uint8_t *)((uintptr_t)XIP_BASE +
                              (uintptr_t)MICRONES_PICO_FLASH_CACHE_OFFSET +
                              (uintptr_t)de->data_offset);

    *out_buf  = (uint8_t *)rom_data;
    *out_size = de->data_size;
    return true;
}

static void fs_free_buf(RomSource *self, uint8_t *buf) {
    (void)self;
    (void)buf;
}

static void fs_refresh(RomSource *self) {
    (void)self;
    load_directory();
}

static bool fs_save_load(RomSource *self, size_t index,
                         uint8_t *wram, size_t wram_size) {
    (void)self; (void)index; (void)wram; (void)wram_size;
    return false;
}

static bool fs_save_store(RomSource *self, size_t index,
                          const uint8_t *wram, size_t wram_size) {
    (void)self; (void)index; (void)wram; (void)wram_size;
    return false;
}

bool rom_source_flash_fs_init(RomSource *out_source) {
    if (out_source == NULL) return false;
    if (MICRONES_PICO_FLASH_CACHE_SIZE == 0u) {
        set_error("flash cache region not configured");
        return false;
    }

    uintptr_t image_end = (uintptr_t)&__flash_binary_end - (uintptr_t)XIP_BASE;
    if ((uint32_t)image_end > (uint32_t)MICRONES_PICO_FLASH_CACHE_OFFSET) {
        set_error("firmware overlaps flash cache (%u > %u)",
                  (unsigned)image_end,
                  (unsigned)MICRONES_PICO_FLASH_CACHE_OFFSET);
        return false;
    }

    memset(&s_state, 0, sizeof(s_state));
    memset(out_source, 0, sizeof(*out_source));

    out_source->user       = &s_state;
    out_source->count      = fs_count;
    out_source->entry      = fs_entry;
    out_source->load       = fs_load;
    out_source->free_buf   = fs_free_buf;
    out_source->refresh    = fs_refresh;
    out_source->save_load  = fs_save_load;
    out_source->save_store = fs_save_store;

    load_directory();
    set_error("");
    return true;
}

const char *rom_source_flash_fs_last_error(void) {
    return s_last_error;
}

void rom_source_flash_fs_set_progress(FlashFsProgressFn fn, void *user) {
    s_progress_fn = fn;
    s_progress_user = user;
}

typedef struct {
    uint32_t write_offset;
    uint8_t  page[FLASH_PAGE_SIZE];
    size_t   page_used;
} FlashWriter;

static void writer_init(FlashWriter *w, uint32_t start_offset) {
    memset(w, 0, sizeof(*w));
    w->write_offset = start_offset;
    memset(w->page, 0xff, sizeof(w->page));
}

static bool writer_flush_page(FlashWriter *w) {
    if (w->page_used == 0) return true;
    while (w->page_used < FLASH_PAGE_SIZE)
        w->page[w->page_used++] = 0xff;
    uint32_t save = save_and_disable_interrupts();
    flash_range_program(w->write_offset, w->page, FLASH_PAGE_SIZE);
    restore_interrupts(save);
    w->write_offset += FLASH_PAGE_SIZE;
    w->page_used = 0;
    memset(w->page, 0xff, sizeof(w->page));
    return true;
}

static bool writer_write(void *user, const uint8_t *data, size_t size) {
    FlashWriter *w = (FlashWriter *)user;
    while (size > 0) {
        size_t space = FLASH_PAGE_SIZE - w->page_used;
        size_t copy = size < space ? size : space;
        memcpy(&w->page[w->page_used], data, copy);
        w->page_used += copy;
        data += copy;
        size -= copy;
        if (w->page_used == FLASH_PAGE_SIZE) {
            if (!writer_flush_page(w)) return false;
        }
    }
    return true;
}

bool rom_source_flash_fs_copy_from(RomSource *self, RomSource *sd_source) {
    (void)self;
    if (sd_source == NULL) {
        set_error("no SD source");
        return false;
    }
    if (MICRONES_PICO_FLASH_CACHE_SIZE == 0u) {
        set_error("flash region not configured");
        return false;
    }

    size_t sd_count = sd_source->count(sd_source);
    if (sd_count == 0) {
        set_error("no ROMs on SD card");
        return false;
    }
    if (sd_count > FLASH_FS_MAX_ROMS)
        sd_count = FLASH_FS_MAX_ROMS;

    FlashFsHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, FLASH_FS_MAGIC, 4);
    hdr.version = FLASH_FS_VERSION;

    size_t sd_index_map[FLASH_FS_MAX_ROMS];
    uint32_t data_cursor = FLASH_FS_DIR_SIZE;

    for (size_t i = 0; i < sd_count; ++i) {
        const RomSourceEntry *se = sd_source->entry(sd_source, i);
        if (se == NULL || se->file_size == 0) continue;
        uint32_t aligned_size = align_up_sector(se->file_size);
        if (data_cursor + aligned_size > (uint32_t)MICRONES_PICO_FLASH_CACHE_SIZE)
            break;

        size_t fi = hdr.rom_count;
        sd_index_map[fi] = i;

        FlashFsDirEntry *de = &hdr.entries[fi];
        snprintf(de->name, sizeof(de->name), "%.*s",
                 (int)(sizeof(de->name) - 1), se->name);
        de->data_offset  = data_cursor;
        de->data_size    = se->file_size;
        de->mapper       = se->mapper;
        de->has_battery  = se->has_battery ? 1 : 0;
        de->supported    = se->supported ? 1 : 0;
        de->prg_size     = se->prg_size;
        de->chr_size     = se->chr_size;

        data_cursor += aligned_size;
        ++hdr.rom_count;
    }

    if (hdr.rom_count == 0) {
        set_error("no ROMs fit in flash");
        return false;
    }

    size_t total_sectors = align_up_sector(data_cursor) / FLASH_SECTOR_SIZE;
    size_t total_pages   = (data_cursor + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    size_t progress_total = total_sectors + total_pages + hdr.rom_count;
    size_t progress_done  = 0;

    report_progress(0, progress_total);

    pico_video_backend_suspend_for_flash();

    uint32_t erase_end = align_up_sector(data_cursor);
    for (uint32_t off = 0; off < erase_end; off += FLASH_SECTOR_SIZE) {
        uint32_t save = save_and_disable_interrupts();
        flash_range_erase((uint32_t)MICRONES_PICO_FLASH_CACHE_OFFSET + off,
                          FLASH_SECTOR_SIZE);
        restore_interrupts(save);
        ++progress_done;
        report_progress(progress_done, progress_total);
    }

    FlashWriter writer;
    writer_init(&writer, (uint32_t)MICRONES_PICO_FLASH_CACHE_OFFSET);
    writer_write(&writer, (const uint8_t *)&hdr, sizeof(hdr));
    writer_flush_page(&writer);

    for (uint8_t fi = 0; fi < hdr.rom_count; ++fi) {
        FlashFsDirEntry *de = &hdr.entries[fi];
        size_t sd_idx = sd_index_map[fi];
        writer_init(&writer, (uint32_t)MICRONES_PICO_FLASH_CACHE_OFFSET + de->data_offset);

        if (sd_source->load_stream != NULL) {
            size_t streamed = 0;
            char err[80];
            err[0] = '\0';
            bool ok = sd_source->load_stream(sd_source, sd_idx, writer_write, &writer,
                                             &streamed, err, sizeof(err));
            if (ok) writer_flush_page(&writer);
            if (!ok) {
                pico_video_backend_resume_after_flash();
                set_error("stream ROM %u failed: %s", (unsigned)fi, err);
                return false;
            }
        } else {
            uint8_t *buf = NULL;
            size_t sz = 0;
            char err[80];
            err[0] = '\0';
            if (!sd_source->load(sd_source, sd_idx, &buf, &sz, err, sizeof(err))) {
                pico_video_backend_resume_after_flash();
                set_error("load ROM %u failed: %s", (unsigned)fi, err);
                return false;
            }
            writer_write(&writer, buf, sz);
            writer_flush_page(&writer);
            sd_source->free_buf(sd_source, buf);
        }

        ++progress_done;
        report_progress(progress_done, progress_total);
    }

    pico_video_backend_resume_after_flash();

    load_directory();
    set_error("");
    return true;
}
