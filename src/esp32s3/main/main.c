#include "audio.h"
#include "board.h"
#include "display.h"
#include "nes_hw_controller.h"
#include "ui.h"

// Portable NES core
#include "nes.h"
#include "framebuffer.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "micrones";

// ─────────────────────────────────────────────────────────────
//  Embedded ROM (injected by CMakeLists.txt via EMBED_FILES)
// ─────────────────────────────────────────────────────────────
#if MICRONES_HAS_ROM
extern const uint8_t micrones_rom_start[] asm(MICRONES_ROM_START_SYMBOL);
extern const uint8_t micrones_rom_end[]   asm(MICRONES_ROM_END_SYMBOL);
#define ROM_START  micrones_rom_start
#define ROM_SIZE   ((size_t)(micrones_rom_end - micrones_rom_start))
#endif

// ─────────────────────────────────────────────────────────────
//  NES state (static – too large for the default stack)
// ─────────────────────────────────────────────────────────────
static Nes s_nes;

enum {
    DISPLAY_FRAME_BUFFER_COUNT = 2,
};

static NesFrameBuffer *s_display_frames[DISPLAY_FRAME_BUFFER_COUNT];
static SemaphoreHandle_t s_display_frame_free[DISPLAY_FRAME_BUFFER_COUNT];
static QueueHandle_t s_display_frame_queue;

// ─────────────────────────────────────────────────────────────
//  Frame timing
// ─────────────────────────────────────────────────────────────
#define TARGET_FRAME_US  16667u   // ~60 fps

static bool display_frames_init(void)
{
    for (unsigned i = 0; i < DISPLAY_FRAME_BUFFER_COUNT; ++i) {
        s_display_frames[i] = (NesFrameBuffer *)heap_caps_malloc(sizeof(NesFrameBuffer),
                                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_display_frames[i] == NULL) {
            s_display_frames[i] = (NesFrameBuffer *)malloc(sizeof(NesFrameBuffer));
        }
        if (s_display_frames[i] == NULL) {
            ESP_LOGE(TAG, "Failed to allocate display frame buffer %u", i);
            return false;
        }
        memset(s_display_frames[i], 0, sizeof(NesFrameBuffer));

        s_display_frame_free[i] = xSemaphoreCreateBinary();
        if (s_display_frame_free[i] == NULL) {
            ESP_LOGE(TAG, "Failed to create display semaphore %u", i);
            return false;
        }
        xSemaphoreGive(s_display_frame_free[i]);
    }

    s_display_frame_queue = xQueueCreate(DISPLAY_FRAME_BUFFER_COUNT, sizeof(uint32_t));
    if (s_display_frame_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create display frame queue");
        return false;
    }

    ESP_LOGI(TAG, "display frame buffers: %u x %u bytes, free internal heap: %lu bytes",
             (unsigned)DISPLAY_FRAME_BUFFER_COUNT,
             (unsigned)sizeof(NesFrameBuffer),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

static uint32_t display_acquire_frame_buffer(uint32_t preferred_index)
{
    uint32_t index = preferred_index % DISPLAY_FRAME_BUFFER_COUNT;

    while (true) {
        for (unsigned attempt = 0; attempt < DISPLAY_FRAME_BUFFER_COUNT; ++attempt) {
            uint32_t candidate = (index + attempt) % DISPLAY_FRAME_BUFFER_COUNT;
            if (xSemaphoreTake(s_display_frame_free[candidate], 0) == pdTRUE) {
                return candidate;
            }
        }

        xSemaphoreTake(s_display_frame_free[index], portMAX_DELAY);
        return index;
    }
}

static void display_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t frame_index = 0;
        if (xQueueReceive(s_display_frame_queue, &frame_index, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const NesFrameBuffer *fb = s_display_frames[frame_index];
        display_stream_begin(NES_DISPLAY_X, NES_DISPLAY_Y,
                             NES_DISPLAY_W, NES_DISPLAY_H);
        for (int y = 0; y < NES_FRAME_HEIGHT; ++y) {
            const uint8_t *row = nes_framebuffer_scanline_const(fb, (uint16_t)y);
            display_stream_indexed_row(row, NES_FRAME_WIDTH);
        }
        display_stream_end();

        xSemaphoreGive(s_display_frame_free[frame_index]);
    }
}

typedef struct {
    uint32_t nes_us;
    uint32_t audio_us;
} StepTimes;

static void print_diag(uint64_t period_us, uint32_t frames,
                       uint32_t audio_pushed, uint32_t audio_skipped, uint32_t audio_overflow,
                       const StepTimes *avg, uint32_t insn_per_frame)
{
    double fps       = (frames * 1000000.0) / (double)period_us;
    double frame_ms  = (double)period_us / (frames * 1000.0);
    ESP_LOGI(TAG,
        "frames=%lu fps=%.1f frame_ms=%.2f | nes=%luus audio=%luus insn/frame=%lu | "
        "audio_pushed=%lu audio_skipped=%lu audio_overflow=%lu audio_free=%u",
        (unsigned long)frames, fps, frame_ms,
        (unsigned long)avg->nes_us,
        (unsigned long)avg->audio_us,
        (unsigned long)insn_per_frame,
        (unsigned long)audio_pushed,
        (unsigned long)audio_skipped,
        (unsigned long)audio_overflow,
        (unsigned)audio_free_slots());
}

// ─────────────────────────────────────────────────────────────
//  Emulator task  (runs on Core 1, large stack)
// ─────────────────────────────────────────────────────────────
static void emulator_task(void *arg)
{
    (void)arg;

    // Allow the USB-JTAG serial CDC to enumerate on the host side before we
    // start printing.  Without this, early log messages are lost because the
    // host hasn't opened the port yet after a firmware reset.
    vTaskDelay(pdMS_TO_TICKS(1500));

    ESP_LOGI(TAG, "free internal heap: %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (!display_frames_init()) {
        goto idle;
    }

    // ── Initialise hardware ──────────────────────────────────
    ESP_LOGI(TAG, "display_init...");
    if (!display_init()) {
        ESP_LOGE(TAG, "display_init failed – halting");
        vTaskSuspend(NULL);
    }
    ESP_LOGI(TAG, "display_init OK");


    ESP_LOGI(TAG, "audio_init...");
    audio_init(48000);  // APU outputs at 48 kHz; I2S runs at native rate, no resampling needed
    ESP_LOGI(TAG, "audio_init OK");

    ESP_LOGI(TAG, "nes_hw_controller_init...");
    nes_hw_controller_init();
    ESP_LOGI(TAG, "nes_hw_controller_init OK");

    // ── Draw static UI overlay ───────────────────────────────
    ESP_LOGI(TAG, "drawing UI overlay...");
    ui_draw_overlay();
    ESP_LOGI(TAG, "UI overlay done");

    if (xTaskCreatePinnedToCore(
        display_task,
        "display",
        6 * 1024,
        NULL,
        4,
        NULL,
        0
    ) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task");
        goto idle;
    }

    // ── Load ROM ─────────────────────────────────────────────
    nes_init(&s_nes);

#if MICRONES_HAS_ROM
    ESP_LOGI(TAG, "Loading embedded ROM (%u bytes)", (unsigned)ROM_SIZE);
    if (!nes_load_cartridge_const_memory(&s_nes, ROM_START, ROM_SIZE)) {
        ESP_LOGE(TAG, "ROM load failed: %s", nes_last_error(&s_nes));
        // Fall through to idle loop
        goto idle;
    }
    nes_reset(&s_nes);
    {
        /* Diagnose whether PRG ROM landed in internal SRAM, PSRAM, or flash.
         *
         * Address ranges on ESP32-S3:
         *   0x3FC00000–0x3FFFFFFF  Internal DRAM  (fastest – zero wait states)
         *   0x3C000000–0x3FBFFFFF  External (PSRAM or flash DROM via DCache)
         *
         * With PSRAM enabled both PSRAM and flash map into the 0x3C… window,
         * so we cannot distinguish them by address alone.  Use heap_caps to
         * ask whether the pointer belongs to the internal-RAM heap instead. */
        const uint8_t *prg = s_nes.cartridge.prg_rom;
        const char *prg_location;
        if ((uintptr_t)prg >= 0x3FC00000u) {
            prg_location = "internal SRAM – fast";
        } else if (heap_caps_check_integrity_addr((intptr_t)prg, false)) {
            /* heap_caps_check_integrity_addr returns true if the address is
             * inside a registered heap region – i.e. it was malloc'd (PSRAM
             * or internal), not a raw flash pointer. */
            prg_location = "PSRAM heap – medium";
        } else {
            prg_location = "FLASH – slow, expect lower fps";
        }
        ESP_LOGI(TAG, "PRG ROM @ %p (%s, %u bytes)",
                 (void *)prg, prg_location,
                 (unsigned)s_nes.cartridge.prg_rom_size);

        /* Log chr_row_pixels placement – hot in ppu_render_scanline (33×/scanline). */
        const uint8_t *chr = s_nes.cartridge.chr_row_pixels;
        const char *chr_location;
        if (chr == NULL) {
            chr_location = "NULL (CHR RAM, unused)";
        } else if ((uintptr_t)chr >= 0x3FC00000u) {
            chr_location = "internal SRAM – fast";
        } else if (heap_caps_check_integrity_addr((intptr_t)chr, false)) {
            chr_location = "PSRAM heap – medium";
        } else {
            chr_location = "FLASH – slow";
        }
        ESP_LOGI(TAG, "chr_row_pixels @ %p (%s, %u bytes), free internal: %lu bytes",
                 (void *)chr, chr_location,
                 (unsigned)(s_nes.cartridge.chr_row_count * 8u),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    ESP_LOGI(TAG, "Emulator running");
#else
    ESP_LOGW(TAG, "No ROM embedded. Set MICRONES_ROM_PATH or place a ROM at roms/smb1.nes and rebuild.");
    goto idle;
#endif

    // ── Main emulator loop ───────────────────────────────────
    {
        uint64_t report_start_us = esp_timer_get_time();
        uint32_t report_frames   = 0;
        AudioStats prev_audio_stats = audio_stats_snapshot();
        uint32_t next_display_buffer = 0;
        StepTimes acc = {0};   // accumulated µs per step over report window
        uint32_t prev_insn_count = s_nes.cpu.insn_count;

        while (true) {
            uint64_t frame_start_us = esp_timer_get_time();
            uint64_t t0, t1;

            // 1. Read inputs → NES controller (hardware controller only; touch disabled)
            {
                NesControllerState hw = nes_hw_controller_read();
                nes_set_controller_state(&s_nes, 0, hw);
            }

            // 2. Acquire display buffer and point the PPU render target directly at it,
            //    so nes_step_frame() writes pixels straight into the buffer that Core 0
            //    will stream to the display – no post-frame memcpy required.
            uint32_t display_buffer = display_acquire_frame_buffer(next_display_buffer);
            nes_set_render_target(&s_nes, s_display_frames[display_buffer]);

            // 3. Step one NES frame (renders directly into s_display_frames[display_buffer])
            t0 = esp_timer_get_time();
            if (!nes_step_frame(&s_nes)) {
                ESP_LOGE(TAG, "NES halted: %s",
                         nes_stop_reason_name(s_nes.stop_info.reason));
                xSemaphoreGive(s_display_frame_free[display_buffer]);
                break;
            }
            t1 = esp_timer_get_time();
            acc.nes_us += (uint32_t)(t1 - t0);

            // 4. Queue rendered frame for display (Core 0 will stream it to the panel)
            xQueueSend(s_display_frame_queue, &display_buffer, 0);
            next_display_buffer = (display_buffer + 1u) % DISPLAY_FRAME_BUFFER_COUNT;

            // 5. Drain APU samples into audio ring buffer
            t0 = esp_timer_get_time();
            {
                static int16_t pcm_tmp[256];
                size_t n;
                while ((n = nes_audio_read_samples(&s_nes, pcm_tmp,
                            sizeof(pcm_tmp) / sizeof(pcm_tmp[0]))) > 0) {
                    for (size_t i = 0; i < n; ++i)
                        pcm_tmp[i] = (int16_t)(pcm_tmp[i] * 3 / 4);
                    (void)audio_push_samples(pcm_tmp, n);
                }
            }
            t1 = esp_timer_get_time();
            acc.audio_us += (uint32_t)(t1 - t0);

            report_frames++;

            // 6. Diagnostics every 60 frames
            if (report_frames >= 60u) {
                uint64_t now_us = esp_timer_get_time();
                AudioStats audio_stats = audio_stats_snapshot();
                uint32_t cur_insn_count = s_nes.cpu.insn_count;
                StepTimes avg = {
                    .nes_us   = acc.nes_us   / report_frames,
                    .audio_us = acc.audio_us  / report_frames,
                };
                print_diag(now_us - report_start_us, report_frames,
                           (uint32_t)(audio_stats.pushed_samples - prev_audio_stats.pushed_samples),
                           (uint32_t)(audio_stats.skipped_samples - prev_audio_stats.skipped_samples),
                           (uint32_t)(audio_stats.overflow_samples - prev_audio_stats.overflow_samples),
                           &avg,
                           (uint32_t)((cur_insn_count - prev_insn_count) / report_frames));
                report_start_us  = now_us;
                report_frames    = 0;
                prev_audio_stats = audio_stats;
                prev_insn_count  = cur_insn_count;
                acc = (StepTimes){0};
            }

            // 7. Frame pacing: spin-wait to maintain ~60 fps
            {
                uint64_t elapsed = esp_timer_get_time() - frame_start_us;
                if (elapsed < TARGET_FRAME_US) {
                    uint32_t sleep_us = (uint32_t)(TARGET_FRAME_US - elapsed);
                    // Use vTaskDelay for delays >= 1 ms; busy-wait for sub-ms
                    if (sleep_us >= 1000u) {
                        vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000u));
                    }
                    // Spin for the remainder
                    uint64_t target = frame_start_us + TARGET_FRAME_US;
                    while (esp_timer_get_time() < target) { /* spin */ }
                }
            }
        }
    }

idle:
    ESP_LOGI(TAG, "Entering idle loop");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ─────────────────────────────────────────────────────────────
//  Application entry point
// ─────────────────────────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "microNES ESP32-S3 AMOLED starting");
    ESP_LOGI(TAG, "free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    xTaskCreatePinnedToCore(
        emulator_task,
        "emulator",
        32 * 1024,  // 32 KB stack
        NULL,
        5,          // priority
        NULL,
        1           // pin to Core 1
    );
}
