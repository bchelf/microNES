/*
 * audio_sigma_delta_test.c — host unit test for the portable noise-shaping
 * modulator in src/pico/audio_sigma_delta.h.
 *
 * The Pico firmware that drives this modulator over DMA cannot be compiled or
 * flashed on the host, but the DSP math is pure integer and dependency-free,
 * so we validate the part most likely to harbour a bug here:
 *   - every emitted level stays within [0, wrap]
 *   - the modulator preserves the DC mean (a sigma-delta's defining property):
 *     the average output level equals the ideal level for a DC input, proving
 *     no systematic offset / gain error
 *   - full-scale inputs clamp cleanly without overflow
 *   - output is deterministic
 */

#include "../src/pico/audio_sigma_delta.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL: ");                                                  \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

/* Ideal (un-quantized) output level for a DC input sample. */
static double ideal_level(int32_t wrap, int16_t sample) {
    return (double)audio_sd_sample_to_q16(wrap, sample) / 65536.0;
}

/* Run a constant DC input; check range and DC-mean preservation. */
static void test_dc(int32_t wrap, int32_t osr_shift, int16_t sample) {
    const int32_t osr = (int32_t)1 << osr_shift;
    AudioSigmaDelta sd;
    audio_sd_init(&sd, wrap, osr_shift);

    uint16_t buf[64];
    const int N = 16384;
    double sum = 0.0;
    long count = 0;
    int in_range = 1;

    for (int i = 0; i < N; ++i) {
        audio_sd_expand(&sd, sample, buf);
        for (int32_t k = 0; k < osr; ++k) {
            if (buf[k] > (uint16_t)wrap) in_range = 0;
            sum += buf[k];
            ++count;
        }
    }

    double mean = sum / (double)count;
    double want = ideal_level(wrap, sample);
    CHECK(in_range, "wrap=%d osr=%d dc=%d: level exceeded wrap", wrap, (int)osr, sample);
    /* A 1st-order error-feedback modulator preserves DC to well under 1/10 LSB,
     * including at the full-scale rails. */
    CHECK(fabs(mean - want) < 0.1,
          "wrap=%d osr=%d dc=%d: mean=%.4f want=%.4f (DC mean not preserved)",
          wrap, (int)osr, sample, mean, want);
}

/* Run a full-amplitude sine; check range only (clamping must not overflow). */
static void test_sine(int32_t wrap, int32_t osr_shift) {
    const int32_t osr = (int32_t)1 << osr_shift;
    AudioSigmaDelta sd;
    audio_sd_init(&sd, wrap, osr_shift);

    uint16_t buf[64];
    int in_range = 1;
    for (int i = 0; i < 48000; ++i) {
        double ph = 2.0 * M_PI * 440.0 * (double)i / 48000.0;
        int16_t s = (int16_t)lrint(32767.0 * sin(ph));
        audio_sd_expand(&sd, s, buf);
        for (int32_t k = 0; k < osr; ++k) {
            if (buf[k] > (uint16_t)wrap) in_range = 0;
        }
    }
    CHECK(in_range, "wrap=%d osr=%d sine: level exceeded wrap", wrap, (int)osr);
}

/* Same input twice must produce identical output. */
static void test_determinism(int32_t wrap, int32_t osr_shift) {
    const int32_t osr = (int32_t)1 << osr_shift;
    AudioSigmaDelta a, b;
    audio_sd_init(&a, wrap, osr_shift);
    audio_sd_init(&b, wrap, osr_shift);
    uint16_t ba[64], bb[64];
    int identical = 1;
    for (int i = 0; i < 4096; ++i) {
        int16_t s = (int16_t)((i * 1103515245 + 12345) & 0xffff); /* pseudo-random */
        audio_sd_expand(&a, s, ba);
        audio_sd_expand(&b, s, bb);
        for (int32_t k = 0; k < osr; ++k) {
            if (ba[k] != bb[k]) identical = 0;
        }
    }
    CHECK(identical, "wrap=%d osr=%d: output not deterministic", wrap, (int)osr);
}

int main(void) {
    /* Primary config: 9-bit carrier, OSR=8. */
    const int32_t wrap9 = 511, osr8 = 3;
    /* Fallback config: 8-bit carrier, OSR=16. */
    const int32_t wrap8 = 255, osr16 = 4;

    int16_t dc_values[] = {-32768, -20000, -8000, 0, 1, 8000, 20000, 32767};
    for (size_t i = 0; i < sizeof(dc_values) / sizeof(dc_values[0]); ++i) {
        test_dc(wrap9, osr8, dc_values[i]);
        test_dc(wrap8, osr16, dc_values[i]);
    }
    test_sine(wrap9, osr8);
    test_sine(wrap8, osr16);
    test_determinism(wrap9, osr8);
    test_determinism(wrap8, osr16);

    if (g_failures == 0) {
        printf("audio_sigma_delta_test: OK\n");
        return 0;
    }
    printf("audio_sigma_delta_test: %d failure(s)\n", g_failures);
    return 1;
}
