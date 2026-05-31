/*
 * audio_sigma_delta.h  —  portable oversampling noise-shaping modulator
 *
 * Converts a stream of 48 kHz int16 PCM samples into an OSR-times-faster
 * stream of small unsigned PWM levels (0..wrap), using linear interpolation
 * (anti-imaging) followed by a 1st-order error-feedback sigma-delta modulator.
 *
 * A 1st-order error-feedback loop is unconditionally stable and tracks DC
 * exactly at every level, including the full-scale rails (verified by the host
 * unit test).  A 2nd-order loop would shape ~18 dB harder but winds up against
 * the output clamp at the rails, biasing loud passages — not worth the risk on
 * hardware that can't be bench-measured here.  At OSR=8 the 1st-order shaper
 * still moves ~27 dB of quantization noise out of the audio band, giving the
 * 9-bit carrier roughly 13-14 effective in-band bits.
 *
 * The point: a plain int16 -> N-bit quantization throws the low (16-N) bits
 * away.  Here the quantization error is fed forward and shaped so its energy
 * moves ABOVE the audio band, where the analog RC reconstruction filter and
 * the ear remove it.  Oversampling (updating the PWM level OSR times per input
 * sample) gives the shaper spectral room to dump that noise into.  The net
 * effect is several extra effective bits of in-band resolution from the same
 * coarse PWM carrier.
 *
 * This header is intentionally dependency-free (only <stdint.h>) and contains
 * no Pico SDK / hardware references, so the exact DSP can be unit-tested on the
 * host.  The Pico backend (audio_pwm.c) drives the output levels via DMA; the
 * host test validates the math here.
 *
 * Fixed-point only (no float): this runs at the OSR'd rate (e.g. 384 kHz) and
 * must be cheap and FPU-state-free inside a DMA IRQ.  Levels are carried in
 * Q16 "level units" (an integer level shifted left 16).
 */

#ifndef MICRONES_AUDIO_SIGMA_DELTA_H
#define MICRONES_AUDIO_SIGMA_DELTA_H

#include <stdint.h>

typedef struct {
    int32_t  wrap;        /* max PWM level (e.g. 511 for a 9-bit carrier)      */
    int32_t  osr_shift;   /* log2(OSR); OSR = 1 << osr_shift                    */
    int32_t  prev_in_q16; /* previous input mapped to Q16 level (interpolation)*/
    int32_t  err;         /* error-feedback accumulator (Q16)                  */
    int16_t  last_input;  /* last input sample (held during underrun)          */
    uint16_t last_level;  /* last emitted level (diagnostics / sample-and-hold)*/
} AudioSigmaDelta;

/* Map an int16 sample [-32768..32767] to a Q16 level in [0 .. wrap<<16]. */
static inline int32_t audio_sd_sample_to_q16(int32_t wrap, int16_t sample) {
    int32_t offset = (int32_t)sample + 32768;             /* [0..65535]        */
    return (int32_t)(((int64_t)offset * ((int64_t)wrap << 16)) / 65535);
}

/* Initialise to mid-scale silence.  osr_shift = log2(oversampling ratio). */
static inline void audio_sd_init(AudioSigmaDelta *sd, int32_t wrap, int32_t osr_shift) {
    sd->wrap        = wrap;
    sd->osr_shift   = osr_shift;
    sd->prev_in_q16 = audio_sd_sample_to_q16(wrap, 0); /* silence == mid-scale */
    sd->err         = 0;
    sd->last_input  = 0;
    sd->last_level  = (uint16_t)((wrap + 1) / 2);
}

/*
 * Expand one int16 input sample into (1 << osr_shift) PWM levels written to
 * dst[0 .. OSR-1].  The caller must size dst for at least OSR entries.
 *
 * Per output step:
 *   x   = linear-interpolated target between previous and current input
 *   u   = x + err               (1st-order error feedback, NTF (1 - z^-1))
 *   lvl = round(u) clamped to [0,wrap]   (clamp == the modulator stability guard)
 *   err = u - lvl               (quantizer residue still owed, carried forward)
 *
 * Sign matters: err is the quantizer INPUT minus its OUTPUT, added back on the
 * next step.  That makes the output telescope to sum(x) + err_0 - err_N, so the
 * DC mean is preserved to under 1/N LSB (verified by the host test).  The
 * opposite sign turns the loop into positive feedback and leaves a ~0.5 LSB DC
 * offset.
 */
static inline void audio_sd_expand(AudioSigmaDelta *sd, int16_t sample, uint16_t *dst) {
    const int32_t osr     = (int32_t)1 << sd->osr_shift;
    const int32_t wrap    = sd->wrap;
    const int32_t cur_q16 = audio_sd_sample_to_q16(wrap, sample);
    const int32_t step    = (cur_q16 - sd->prev_in_q16) >> sd->osr_shift;
    int32_t       x       = sd->prev_in_q16;
    int32_t       err     = sd->err;

    for (int32_t k = 0; k < osr; ++k) {
        x += step;
        int32_t u   = x + err;                         /* shaped value (Q16)   */
        int32_t lvl = (u + (1 << 15)) >> 16;           /* round to integer lvl */
        if (lvl < 0) {
            lvl = 0;
        } else if (lvl > wrap) {
            lvl = wrap;
        }
        err    = u - (lvl << 16);                      /* residue (Q16)        */
        dst[k] = (uint16_t)lvl;
    }

    sd->prev_in_q16 = cur_q16;
    sd->err         = err;
    sd->last_input  = sample;
    sd->last_level  = dst[osr - 1];
}

#endif /* MICRONES_AUDIO_SIGMA_DELTA_H */
