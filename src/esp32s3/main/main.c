#include "audio.h"
#include "board.h"
#include "display.h"
#include "nes_input.h"
#include "nes_video.h"
#include "touch.h"
#include "ui.h"

// Portable NES core
#include "nes.h"
#include "framebuffer.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "micrones";

// ─────────────────────────────────────────────────────────────
//  Embedded ROM (injected by CMakeLists.txt via EMBED_FILES)
// ─────────────────────────────────────────────────────────────
#if MICRONES_HAS_ROM
// Symbol names are derived from the filename: roms/smb1.nes → smb1_nes
extern const uint8_t    _binary_smb1_nes_start[];
extern const uint8_t    _binary_smb1_nes_end[];
#define ROM_START  _binary_smb1_nes_start
#define ROM_SIZE   ((size_t)(_binary_smb1_nes_end - _binary_smb1_nes_start))
#endif

// ─────────────────────────────────────────────────────────────
//  NES state (static – too large for the default stack)
// ─────────────────────────────────────────────────────────────
static Nes s_nes;

// Full-frame RGB565 buffer (256×240 × 2 bytes = 120 KB) in internal SRAM.
// DMA_ATTR ensures 4-byte alignment so the SPI DMA engine can read it directly.
static DMA_ATTR uint16_t s_frame_rgb[NES_FRAME_WIDTH * NES_FRAME_HEIGHT];

// ─────────────────────────────────────────────────────────────
//  Frame timing
// ─────────────────────────────────────────────────────────────
#define TARGET_FRAME_US  16667u   // ~60 fps

// ─────────────────────────────────────────────────────────────
//  Performance diagnostics (printed every 60 frames)
// ─────────────────────────────────────────────────────────────
typedef struct {
    uint64_t nes_us;      // nes_step_frame
    uint64_t conv_us;     // palette conversion loop
    uint64_t blit_us;     // display_blit_region
    uint64_t audio_us;    // audio drain
    uint64_t touch_us;    // touch_read
} FrameStageUs;

static void print_diag(uint64_t period_us, uint32_t frames,
                       uint32_t audio_pushed, uint32_t audio_dropped,
                       const FrameStageUs *stage_sum)
{
    double fps       = (frames * 1000000.0) / (double)period_us;
    double frame_ms  = (double)period_us / (frames * 1000.0);
    double div       = frames * 1000.0;
    ESP_LOGI(TAG,
        "frames=%lu fps=%.1f frame_ms=%.2f "
        "nes=%.2f conv=%.2f blit=%.2f audio=%.2f touch=%.2f "
        "audio_pushed=%lu audio_dropped=%lu audio_free=%u",
        (unsigned long)frames, fps, frame_ms,
        (double)stage_sum->nes_us   / div,
        (double)stage_sum->conv_us  / div,
        (double)stage_sum->blit_us  / div,
        (double)stage_sum->audio_us / div,
        (double)stage_sum->touch_us / div,
        (unsigned long)audio_pushed, (unsigned long)audio_dropped,
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

    // ── Initialise hardware ──────────────────────────────────
    ESP_LOGI(TAG, "display_init...");
    if (!display_init()) {
        ESP_LOGE(TAG, "display_init failed – halting");
        vTaskSuspend(NULL);
    }
    ESP_LOGI(TAG, "display_init OK");

    ESP_LOGI(TAG, "touch_init...");
    if (!touch_init()) {
        ESP_LOGW(TAG, "touch_init failed – touch input disabled");
    } else {
        ESP_LOGI(TAG, "touch_init OK");
    }

    ESP_LOGI(TAG, "audio_init...");
    audio_init(48000);
    ESP_LOGI(TAG, "audio_init OK");

    // ── Draw static UI overlay ───────────────────────────────
    ESP_LOGI(TAG, "drawing UI overlay...");
    ui_draw_overlay();
    ESP_LOGI(TAG, "UI overlay done");

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
        ESP_LOGI(TAG, "PRG ROM @ %p (%s, %u bytes), free heap: %lu bytes",
                 (void *)prg, prg_location,
                 (unsigned)s_nes.cartridge.prg_rom_size,
                 (unsigned long)esp_get_free_heap_size());
    }
    ESP_LOGI(TAG, "Emulator running");
#else
    ESP_LOGW(TAG, "No ROM embedded.  Place roms/smb1.nes in the repo root and rebuild.");
    goto idle;
#endif

    // ── Main emulator loop ───────────────────────────────────
    {
        uint64_t report_start_us = esp_timer_get_time();
        uint32_t report_frames   = 0;
        uint32_t report_pushed   = 0;
        uint32_t report_dropped  = 0;
        FrameStageUs stage_sum   = {0};

        while (true) {
            uint64_t frame_start_us = esp_timer_get_time();
            uint64_t t0, t1;

            // 1. Read touch → NES controller
            t0 = esp_timer_get_time();
            {
                TouchData td;
                touch_read(&td);
                NesControllerState state = nes_input_from_touch(&td);
                nes_set_controller_state(&s_nes, 0, state);
            }
            stage_sum.touch_us += esp_timer_get_time() - t0;

            // 2. Step one NES frame
            t0 = esp_timer_get_time();
            if (!nes_step_frame(&s_nes)) {
                ESP_LOGE(TAG, "NES halted: %s",
                         nes_stop_reason_name(s_nes.stop_info.reason));
                break;
            }
            stage_sum.nes_us += esp_timer_get_time() - t0;

            // 3. Convert palette indices → RGB565 then blit to display
            t0 = esp_timer_get_time();
            {
                const NesFrameBuffer *fb = nes_framebuffer(&s_nes);
                for (int y = 0; y < NES_FRAME_HEIGHT; y++) {
                    const uint8_t *row = nes_framebuffer_scanline_const(fb, (uint16_t)y);
                    nes_video_convert_scanline(row, &s_frame_rgb[y * NES_FRAME_WIDTH]);
                }
            }
            t1 = esp_timer_get_time();
            stage_sum.conv_us += t1 - t0;

            display_blit_region(NES_DISPLAY_X, NES_DISPLAY_Y,
                                NES_DISPLAY_W, NES_DISPLAY_H,
                                s_frame_rgb);
            stage_sum.blit_us += esp_timer_get_time() - t1;

            // 4. Drain APU samples into audio ring buffer
            t0 = esp_timer_get_time();
            {
                static int16_t pcm_tmp[256];
                size_t n;
                while ((n = nes_audio_read_samples(&s_nes, pcm_tmp,
                            sizeof(pcm_tmp) / sizeof(pcm_tmp[0]))) > 0) {
                    size_t pushed = audio_push_samples(pcm_tmp, n);
                    report_pushed  += (uint32_t)n;
                    report_dropped += (uint32_t)(n - pushed);
                }
            }
            stage_sum.audio_us += esp_timer_get_time() - t0;

            report_frames++;

            // 5. Diagnostics every 60 frames
            if (report_frames >= 60u) {
                uint64_t now_us = esp_timer_get_time();
                print_diag(now_us - report_start_us, report_frames,
                           report_pushed, report_dropped, &stage_sum);
                report_start_us  = now_us;
                report_frames    = 0;
                report_pushed    = 0;
                report_dropped   = 0;
                memset(&stage_sum, 0, sizeof(stage_sum));
            }

            // 6. Frame pacing: spin-wait to maintain ~60 fps
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
