#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t blit_calls;
    uint64_t blit_chunks;
    uint64_t blit_copy_us;
    uint64_t blit_tx_us;
    uint64_t blit_bytes;
} DisplayProfile;

// Initialise the SPI bus and SH8601 AMOLED panel.
// After this call the display is on, rotated to landscape (536×240),
// and the screen content is undefined.
bool display_init(void);

// Fill the entire display with a single colour (RGB565, big-endian byte order).
void display_fill(uint16_t rgb565_be);

// Write an arbitrary rectangular region.
//   x, y  – top-left corner in landscape coordinates (0-based)
//   w, h  – width and height in pixels
//   data  – row-major array of w*h RGB565 values (big-endian byte order)
void display_write_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          const uint16_t *data);

// Write a single row of pixels into the display at (x, row_y), length w.
// Useful for scanline-by-scanline NES updates.
void display_write_row(uint16_t x, uint16_t row_y, uint16_t w,
                       const uint16_t *row);

// Fast path for DMA-accessible (internal SRAM) buffers: sends w*h pixels in a
// single SPI transaction with no intermediate copy.  buf must be in internal SRAM.
void display_blit_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint16_t *buf);

// Convenience: set the write window once, then stream rows via
// display_stream_row().  Call display_stream_begin() first,
// then display_stream_row() for each row, then display_stream_end().
void display_stream_begin(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void display_stream_row(const uint16_t *row, uint16_t w);
void display_stream_indexed_row(const uint8_t *palette_row, uint16_t w);
void display_stream_end(void);

DisplayProfile display_profile_snapshot(void);
