#include "emulator_video_adapter.h"

#include "display/video_tft.h"

#include "pico/time.h"

#include <stdio.h>
#include <string.h>

#if !MICRONES_ENABLE_FRAMEBUFFER
#error "The TFT Pico target requires MICRONES_ENABLE_FRAMEBUFFER=1"
#endif

static void emulator_video_adapter_set_error(PicoEmulatorVideoAdapter *adapter, const char *message) {
    snprintf(adapter->last_error, sizeof(adapter->last_error), "%s", message);
}

static uint64_t emulator_video_adapter_now_us(void *user) {
    (void)user;
    return time_us_64();
}

bool emulator_video_adapter_init(
    PicoEmulatorVideoAdapter *adapter,
    const uint8_t *rom_image,
    size_t rom_image_size
) {
    memset(adapter, 0, sizeof(*adapter));
    nes_init(&adapter->nes);

    if (!nes_load_cartridge_memory(&adapter->nes, rom_image, rom_image_size)) {
        emulator_video_adapter_set_error(adapter, nes_last_error(&adapter->nes));
        nes_destroy(&adapter->nes);
        return false;
    }

    nes_set_profile_clock(&adapter->nes, emulator_video_adapter_now_us, NULL);
    nes_reset(&adapter->nes);
    adapter->initialized = true;
    adapter->debug_overlay_enabled = false;
    adapter->last_frame_first_visible_x = -1;
    adapter->last_frame_first_visible_y = -1;
    emulator_video_adapter_set_error(adapter, "");
    return true;
}

bool emulator_video_adapter_init_empty(PicoEmulatorVideoAdapter *adapter) {
    memset(adapter, 0, sizeof(*adapter));
    nes_init(&adapter->nes);
    nes_set_profile_clock(&adapter->nes, emulator_video_adapter_now_us, NULL);
    adapter->initialized = true;
    adapter->last_frame_first_visible_x = -1;
    adapter->last_frame_first_visible_y = -1;
    emulator_video_adapter_set_error(adapter, "");
    return true;
}

void emulator_video_adapter_present_framebuffer(
    PicoEmulatorVideoAdapter *adapter,
    const NesFrameBuffer *fb
) {
    if (fb == NULL) return;
    uint64_t t0 = time_us_64();
    video_tft_present_frame(fb);
    adapter->profile_render_frame_us_total += time_us_64() - t0;
}

bool emulator_video_adapter_step_frame(PicoEmulatorVideoAdapter *adapter) {
    uint64_t t0;

    if (!adapter->initialized) {
        emulator_video_adapter_set_error(adapter, "emulator video adapter is not initialized");
        return false;
    }

    t0 = time_us_64();
    if (!nes_step_frame(&adapter->nes)) {
        emulator_video_adapter_set_error(adapter, nes_last_error(&adapter->nes));
        return false;
    }
    adapter->profile_step_scanline_us_total += time_us_64() - t0;
    ++adapter->rendered_frames;
    adapter->rendered_scanlines += NES_FRAME_HEIGHT;
    return true;
}

void emulator_video_adapter_present_frame(PicoEmulatorVideoAdapter *adapter) {
    uint64_t t0 = time_us_64();
    const NesFrameBuffer *frame = nes_framebuffer(&adapter->nes);

    /* NOTE: The debug pixel-scan loop that previously ran here iterated all
     * 256×240 = 61,440 pixels to compute color_mask, min/max pixel, and
     * first-visible coordinates.  It used 64-bit shift ops and multiple
     * branches per pixel, costing ~2–4 ms per frame at 315 MHz — pure
     * overhead that contributed nothing to rendering.  Removed. */
    video_tft_present_frame(frame);
    adapter->profile_render_frame_us_total += time_us_64() - t0;

    /* Debug pixel stats not collected on TFT path (scan removed above). */
    adapter->last_frame_source_nonzero_pixels = 0;
    adapter->last_frame_visible_nonblack_pixels = 0;
    adapter->last_frame_visible_white_pixels = 0;
    adapter->last_frame_visible_gray_pixels = 0;
    adapter->last_frame_min_pixel = 0;
    adapter->last_frame_max_pixel = 0;
    adapter->last_frame_unique_color_count = 0;
    adapter->last_frame_first_visible_x = -1;
    adapter->last_frame_first_visible_y = -1;
}

bool emulator_video_adapter_render_frame(PicoEmulatorVideoAdapter *adapter) {
    if (!emulator_video_adapter_step_frame(adapter)) {
        return false;
    }
    emulator_video_adapter_present_frame(adapter);
    return true;
}

const char *emulator_video_adapter_last_error(const PicoEmulatorVideoAdapter *adapter) {
    return adapter->last_error;
}
