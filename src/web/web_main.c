#include "nes.h"

#include <emscripten.h>
#include <stddef.h>
#include <stdint.h>

static Nes g_nes;
static int g_initialized = 0;

EMSCRIPTEN_KEEPALIVE void nes_web_init(void) {
    nes_init(&g_nes);
    g_initialized = 1;
}

EMSCRIPTEN_KEEPALIVE int nes_web_load_rom(const uint8_t *data, size_t size) {
    if (!g_initialized) {
        nes_init(&g_nes);
        g_initialized = 1;
    }
    if (!nes_load_cartridge_memory(&g_nes, data, size)) {
        return 0;
    }
    nes_reset(&g_nes);
    return 1;
}

EMSCRIPTEN_KEEPALIVE void nes_web_reset(void) {
    nes_reset(&g_nes);
}

EMSCRIPTEN_KEEPALIVE int nes_web_step_frame(void) {
    return nes_step_frame(&g_nes) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const uint8_t *nes_web_framebuffer_ptr(void) {
    return nes_framebuffer(&g_nes)->pixels;
}

EMSCRIPTEN_KEEPALIVE uint32_t nes_web_audio_sample_rate(void) {
    return nes_audio_sample_rate(&g_nes);
}

EMSCRIPTEN_KEEPALIVE size_t nes_web_audio_read_samples(int16_t *dst, size_t max_samples) {
    return nes_audio_read_samples(&g_nes, dst, max_samples);
}

EMSCRIPTEN_KEEPALIVE void nes_web_set_controller(uint8_t buttons) {
    NesControllerState state;
    state.buttons = buttons;
    nes_set_controller_state(&g_nes, 0, state);
}

EMSCRIPTEN_KEEPALIVE const char *nes_web_last_error(void) {
    return nes_last_error(&g_nes);
}
