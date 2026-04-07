#ifndef MICRONES_DISPLAY_PARALLEL_8080_H
#define MICRONES_DISPLAY_PARALLEL_8080_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool parallel_tft_init(void);
const char *parallel_tft_last_error(void);
void parallel_tft_write_command(uint8_t cmd);
void parallel_tft_write_data_blocking(const uint8_t *data, size_t len);
void parallel_tft_begin_pixels(void);
void parallel_tft_write_pixels_blocking(const uint8_t *data, size_t len);
void parallel_tft_write_pixels_dma(const uint8_t *data, size_t len);
void parallel_tft_wait_for_completion(void);
void parallel_tft_end_pixels(void);
void parallel_tft_set_window(int x0, int y0, int x1, int y1);
void parallel_tft_blit_rect_rgb565(int x, int y, int w, int h, const uint16_t *src, int stride);

#endif
