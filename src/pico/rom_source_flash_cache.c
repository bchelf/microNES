#include "rom_source_flash_cache.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "pico_video_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#ifndef MICRONES_PICO_ENABLE_FLASH_ROM_CACHE
#define MICRONES_PICO_ENABLE_FLASH_ROM_CACHE 0
#endif

#ifndef MICRONES_PICO_FLASH_CACHE_OFFSET
#define MICRONES_PICO_FLASH_CACHE_OFFSET 0
#endif

#ifndef MICRONES_PICO_FLASH_CACHE_SIZE
#define MICRONES_PICO_FLASH_CACHE_SIZE 0
#endif

typedef struct {
    RomSource *backing;
} FlashCacheState;

typedef struct {
    uint32_t write_offset;
    uint8_t page[FLASH_PAGE_SIZE];
    size_t page_used;
    size_t bytes_programmed;
    size_t progress_base;
    size_t progress_total;
} FlashCacheWriter;

typedef struct {
    const uint8_t *xip;
    size_t offset;
    bool mismatch;
} FlashCacheCompare;

static FlashCacheState s_state;
static char s_last_error[160];
static FlashCacheProgressFn s_progress_fn;
static void *s_progress_user;

extern char __flash_binary_end;

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
}

static uint32_t align_up_u32(uint32_t value, uint32_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

static bool validate_cache_range(size_t rom_size, char *err, size_t err_size) {
    uintptr_t image_end = (uintptr_t)&__flash_binary_end - (uintptr_t)XIP_BASE;

    if (MICRONES_PICO_FLASH_CACHE_SIZE == 0u) {
        if (err && err_size) snprintf(err, err_size, "flash cache disabled");
        return false;
    }
    if ((uint32_t)image_end > (uint32_t)MICRONES_PICO_FLASH_CACHE_OFFSET) {
        if (err && err_size) {
            snprintf(err, err_size, "firmware overlaps flash cache (%u > %u)",
                     (unsigned)image_end,
                     (unsigned)MICRONES_PICO_FLASH_CACHE_OFFSET);
        }
        return false;
    }
    if (rom_size > (size_t)MICRONES_PICO_FLASH_CACHE_SIZE) {
        if (err && err_size) {
            snprintf(err, err_size, "ROM too large for flash cache (%u > %u)",
                     (unsigned)rom_size,
                     (unsigned)MICRONES_PICO_FLASH_CACHE_SIZE);
        }
        return false;
    }
    return true;
}

static void report_progress(size_t done, size_t total) {
    if (s_progress_fn != NULL) {
        s_progress_fn(done, total, s_progress_user);
    }
}

static bool flash_cache_program_page(FlashCacheWriter *writer) {
    uint32_t save = save_and_disable_interrupts();
    flash_range_program(writer->write_offset, writer->page, FLASH_PAGE_SIZE);
    restore_interrupts(save);
    writer->write_offset += FLASH_PAGE_SIZE;
    writer->bytes_programmed += FLASH_PAGE_SIZE;
    writer->page_used = 0;
    memset(writer->page, 0xff, sizeof(writer->page));

    size_t pages_done = writer->bytes_programmed / FLASH_PAGE_SIZE;
    report_progress(writer->progress_base + pages_done, writer->progress_total);

    return true;
}

static bool flash_cache_write(void *user, const uint8_t *data, size_t size) {
    FlashCacheWriter *writer = (FlashCacheWriter *)user;

    while (size > 0) {
        size_t space = FLASH_PAGE_SIZE - writer->page_used;
        size_t copy = size < space ? size : space;
        memcpy(&writer->page[writer->page_used], data, copy);
        writer->page_used += copy;
        data += copy;
        size -= copy;

        if (writer->page_used == FLASH_PAGE_SIZE) {
            flash_cache_program_page(writer);
        }
    }
    return true;
}

static bool flash_cache_flush(FlashCacheWriter *writer) {
    if (writer->page_used == 0) {
        return true;
    }
    return flash_cache_program_page(writer);
}

static bool flash_cache_compare_write(void *user, const uint8_t *data, size_t size) {
    FlashCacheCompare *cmp = (FlashCacheCompare *)user;

    if (memcmp(cmp->xip + cmp->offset, data, size) != 0) {
        cmp->mismatch = true;
        return false;
    }
    cmp->offset += size;
    return true;
}

static size_t cached_count(RomSource *self) {
    FlashCacheState *st = (FlashCacheState *)self->user;
    return (st->backing != NULL && st->backing->count != NULL)
        ? st->backing->count(st->backing)
        : 0u;
}

static const RomSourceEntry *cached_entry(RomSource *self, size_t index) {
    FlashCacheState *st = (FlashCacheState *)self->user;
    return (st->backing != NULL && st->backing->entry != NULL)
        ? st->backing->entry(st->backing, index)
        : NULL;
}

static bool cached_load(RomSource *self, size_t index,
                        uint8_t **out_buf, size_t *out_size,
                        char *err, size_t err_size) {
#if MICRONES_PICO_ENABLE_FLASH_ROM_CACHE
    FlashCacheState *st = (FlashCacheState *)self->user;
    const RomSourceEntry *entry = cached_entry(self, index);
    size_t rom_size = entry != NULL ? entry->file_size : 0u;
    uint32_t erase_size;
    FlashCacheWriter writer;
    size_t streamed_size = 0;
    bool ok;
    FlashCacheCompare cmp;

    if (out_buf == NULL || out_size == NULL) {
        if (err && err_size) snprintf(err, err_size, "missing output pointer");
        return false;
    }
    if (st->backing == NULL) {
        if (err && err_size) snprintf(err, err_size, "missing backing source");
        return false;
    }
    if (rom_size == 0u) {
        if (err && err_size) snprintf(err, err_size, "unknown ROM size");
        return false;
    }
    if (st->backing->load_stream == NULL) {
        if (err && err_size) snprintf(err, err_size, "backing source cannot stream");
        return false;
    }
    if (!validate_cache_range(rom_size, err, err_size)) {
        return false;
    }

    erase_size = align_up_u32((uint32_t)rom_size, FLASH_SECTOR_SIZE);
    memset(&writer, 0, sizeof(writer));
    writer.write_offset = (uint32_t)MICRONES_PICO_FLASH_CACHE_OFFSET;
    memset(writer.page, 0xff, sizeof(writer.page));

    printf("[flash] load index=%u rom_size=%u erase_size=%u\n",
           (unsigned)index, (unsigned)rom_size, (unsigned)erase_size);

    memset(&cmp, 0, sizeof(cmp));
    cmp.xip = (const uint8_t *)((uintptr_t)XIP_BASE + (uintptr_t)MICRONES_PICO_FLASH_CACHE_OFFSET);
    ok = st->backing->load_stream(st->backing, index, flash_cache_compare_write, &cmp,
                                  &streamed_size, err, err_size);
    if (ok && streamed_size == rom_size && !cmp.mismatch) {
        printf("[flash] cache HIT\n");
        *out_buf = (uint8_t *)cmp.xip;
        *out_size = rom_size;
        set_error("");
        return true;
    }
    if (!cmp.mismatch && !ok) {
        printf("[flash] compare stream failed\n");
        return false;
    }
    if (err != NULL && err_size > 0) {
        err[0] = '\0';
    }
    streamed_size = 0;

    printf("[flash] cache MISS, erasing %u sectors, programming %u pages\n",
           (unsigned)(erase_size / FLASH_SECTOR_SIZE),
           (unsigned)(align_up_u32((uint32_t)rom_size, FLASH_PAGE_SIZE) / FLASH_PAGE_SIZE));

    pico_video_backend_suspend_for_flash();

    uint32_t total_sectors = erase_size / FLASH_SECTOR_SIZE;
    uint32_t total_pages = align_up_u32((uint32_t)rom_size, FLASH_PAGE_SIZE) / FLASH_PAGE_SIZE;
    size_t progress_total = (size_t)total_sectors + (size_t)total_pages;
    size_t progress_done = 0;

    report_progress(0, progress_total);

    uint64_t erase_start_us = time_us_64();
    for (uint32_t off = 0; off < erase_size; off += FLASH_SECTOR_SIZE) {
        uint32_t save = save_and_disable_interrupts();
        flash_range_erase((uint32_t)MICRONES_PICO_FLASH_CACHE_OFFSET + off,
                          FLASH_SECTOR_SIZE);
        restore_interrupts(save);
        ++progress_done;
        report_progress(progress_done, progress_total);
    }
    printf("[flash] erase done in %llu ms\n",
           (time_us_64() - erase_start_us) / 1000u);

    writer.progress_base = (size_t)total_sectors;
    writer.progress_total = progress_total;

    uint64_t prog_start_us = time_us_64();
    ok = st->backing->load_stream(st->backing, index, flash_cache_write, &writer,
                                  &streamed_size, err, err_size);
    if (ok) {
        ok = flash_cache_flush(&writer);
    }
    printf("[flash] program done in %llu ms, streamed=%u/%u ok=%d\n",
           (time_us_64() - prog_start_us) / 1000u,
           (unsigned)streamed_size, (unsigned)rom_size, (int)ok);

    pico_video_backend_resume_after_flash();

    if (!ok) {
        printf("[flash] program FAILED\n");
        return false;
    }
    if (streamed_size != rom_size) {
        printf("[flash] size mismatch: streamed %u expected %u\n",
               (unsigned)streamed_size, (unsigned)rom_size);
        if (err && err_size) {
            snprintf(err, err_size, "streamed %u/%u bytes",
                     (unsigned)streamed_size, (unsigned)rom_size);
        }
        return false;
    }

    printf("[flash] load OK, XIP at 0x%08x, %u bytes\n",
           (unsigned)(XIP_BASE + MICRONES_PICO_FLASH_CACHE_OFFSET),
           (unsigned)rom_size);
    *out_buf = (uint8_t *)((uintptr_t)XIP_BASE + (uintptr_t)MICRONES_PICO_FLASH_CACHE_OFFSET);
    *out_size = rom_size;
    set_error("");
    return true;
#else
    (void)self; (void)index; (void)out_buf; (void)out_size;
    if (err && err_size) snprintf(err, err_size, "flash cache disabled");
    return false;
#endif
}

static void cached_free_buf(RomSource *self, uint8_t *buf) {
    (void)self;
    (void)buf;
}

static void cached_refresh(RomSource *self) {
    FlashCacheState *st = (FlashCacheState *)self->user;
    if (st->backing != NULL && st->backing->refresh != NULL) {
        st->backing->refresh(st->backing);
    }
}

static bool cached_save_load(RomSource *self, size_t index,
                             uint8_t *wram, size_t wram_size) {
    FlashCacheState *st = (FlashCacheState *)self->user;
    return st->backing != NULL && st->backing->save_load != NULL &&
           st->backing->save_load(st->backing, index, wram, wram_size);
}

static bool cached_save_store(RomSource *self, size_t index,
                              const uint8_t *wram, size_t wram_size) {
    FlashCacheState *st = (FlashCacheState *)self->user;
    return st->backing != NULL && st->backing->save_store != NULL &&
           st->backing->save_store(st->backing, index, wram, wram_size);
}

bool rom_source_flash_cache_init(RomSource *out_source, RomSource *backing) {
    if (out_source == NULL || backing == NULL) {
        return false;
    }
    memset(&s_state, 0, sizeof(s_state));
    memset(out_source, 0, sizeof(*out_source));
    s_state.backing = backing;
    out_source->user = &s_state;
    out_source->count = cached_count;
    out_source->entry = cached_entry;
    out_source->load = cached_load;
    out_source->free_buf = cached_free_buf;
    out_source->refresh = cached_refresh;
    out_source->save_load = cached_save_load;
    out_source->save_store = cached_save_store;
    set_error("");
    return true;
}

const char *rom_source_flash_cache_last_error(void) {
    return s_last_error;
}

void rom_source_flash_cache_set_progress(FlashCacheProgressFn fn, void *user) {
    s_progress_fn = fn;
    s_progress_user = user;
}
