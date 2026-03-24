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

#include "esp_log.h"
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

// Per-scanline RGB565 conversion buffer (256 pixels × 2 bytes, SRAM)
// display_stream_row() copies this into the DMA buffer internally.
static uint16_t s_scanline_rgb[NES_FRAME_WIDTH];

// ─────────────────────────────────────────────────────────────
//  Frame timing
// ─────────────────────────────────────────────────────────────
#define TARGET_FRAME_US  16667u   // ~60 fps

// ─────────────────────────────────────────────────────────────
//  Performance diagnostics (printed every 60 frames)
// ─────────────────────────────────────────────────────────────
static void print_diag(uint64_t period_us, uint32_t frames,
                       uint32_t audio_pushed, uint32_t audio_dropped)
{
    double fps       = (frames * 1000000.0) / (double)period_us;
    double frame_ms  = (double)period_us / (frames * 1000.0);
    ESP_LOGI(TAG,
        "frames=%lu fps=%.1f frame_ms=%.2f "
        "audio_pushed=%lu audio_dropped=%lu audio_free=%u",
        (unsigned long)frames, fps, frame_ms,
        (unsigned long)audio_pushed, (unsigned long)audio_dropped,
        (unsigned)audio_free_slots());
}

// ─────────────────────────────────────────────────────────────
//  Emulator task  (runs on Core 1, large stack)
// ─────────────────────────────────────────────────────────────
static void emulator_task(void *arg)
{
    (void)arg;

    // ── Initialise hardware ──────────────────────────────────
    if (!display_init()) {
        ESP_LOGE(TAG, "display_init failed – halting");
        vTaskSuspend(NULL);
    }

    if (!touch_init()) {
        ESP_LOGW(TAG, "touch_init failed – touch input disabled");
    }

    audio_init(48000);

    // ── Draw static UI overlay ───────────────────────────────
    ui_draw_overlay();

    // ── Load ROM ─────────────────────────────────────────────
    nes_init(&s_nes);

#if MICRONES_HAS_ROM
    ESP_LOGI(TAG, "Loading embedded ROM (%u bytes)", (unsigned)ROM_SIZE);
    if (!nes_load_cartridge_memory(&s_nes, ROM_START, ROM_SIZE)) {
        ESP_LOGE(TAG, "ROM load failed: %s", nes_last_error(&s_nes));
        // Fall through to idle loop
        goto idle;
    }
    nes_reset(&s_nes);
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

        while (true) {
            uint64_t frame_start_us = esp_timer_get_time();

            // 1. Read touch → NES controller
            {
                TouchData td;
                touch_read(&td);
                NesControllerState state = nes_input_from_touch(&td);
                nes_set_controller_state(&s_nes, 0, state);
            }

            // 2. Step one NES frame
            if (!nes_step_frame(&s_nes)) {
                ESP_LOGE(TAG, "NES halted: %s",
                         nes_stop_reason_name(s_nes.stop_info.reason));
                break;
            }

            // 3. Blit NES framebuffer to display (NES centre region only)
            {
                const NesFrameBuffer *fb = nes_framebuffer(&s_nes);
                display_stream_begin(NES_DISPLAY_X, NES_DISPLAY_Y,
                                     NES_DISPLAY_W, NES_DISPLAY_H);
                for (int y = 0; y < NES_FRAME_HEIGHT; y++) {
                    const uint8_t *row = nes_framebuffer_scanline_const(fb, (uint16_t)y);
                    nes_video_convert_scanline(row, s_scanline_rgb);
                    display_stream_row(s_scanline_rgb, NES_FRAME_WIDTH);
                }
                display_stream_end();
            }

            // 4. Drain APU samples into audio ring buffer
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

            report_frames++;

            // 5. Diagnostics every 60 frames
            if (report_frames >= 60u) {
                uint64_t now_us = esp_timer_get_time();
                print_diag(now_us - report_start_us, report_frames,
                           report_pushed, report_dropped);
                report_start_us  = now_us;
                report_frames    = 0;
                report_pushed    = 0;
                report_dropped   = 0;
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

    // Create emulator task on Core 1, with a generous stack (NES struct is ~60 KB
    // but most is in .bss; we need stack for nested calls + scanline buffers).
    xTaskCreatePinnedToCore(
        emulator_task,
        "emulator",
        16 * 1024,  // 16 KB stack
        NULL,
        5,          // priority
        NULL,
        1           // pin to Core 1
    );
}
