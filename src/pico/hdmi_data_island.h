#ifndef MICRONES_HDMI_DATA_ISLAND_H
#define MICRONES_HDMI_DATA_ISLAND_H

/*
 * HDMI data-island primitives.
 *
 * Portable C (no Pico SDK deps). The Pico HSTX glue lives in video_hstx.c;
 * this module just produces 32-bit RAW words ready to be fed to the HSTX
 * FIFO via an HSTX_CMD_RAW command.
 *
 * Word layout (matches video_hstx.c blanking sync words):
 *   bits  9: 0  = channel 0 TMDS/TERC4 codeword (10 bits)
 *   bits 19:10  = channel 1
 *   bits 29:20  = channel 2
 *   bits 31:30  = unused (HSTX consumes 30 bits per word)
 */

#include <stddef.h>
#include <stdint.h>

#define HDMI_PACKET_PIXELS         32u
#define HDMI_PACKET_BYTES_HEADER   4u   /* HB0 HB1 HB2 ECC */
#define HDMI_PACKET_BYTES_SUBPKT   8u   /* PB0..PB6 + ECC, four subpackets */
#define HDMI_PACKET_RAW_WORDS      32u  /* one HSTX RAW word per pixel */

#define HDMI_DI_PREAMBLE_PIXELS    8u
#define HDMI_DI_GUARDBAND_PIXELS   2u
#define HDMI_VIDEO_GUARDBAND_PIXELS 2u
#define HDMI_VIDEO_PREAMBLE_PIXELS  8u

typedef struct {
    /* Header: HB0, HB1, HB2 (ECC computed by encoder). */
    uint8_t header[3];
    /* Four subpackets, each 7 payload bytes (ECC computed by encoder). */
    uint8_t subpackets[4][7];
} HdmiPacket;

/* HDMI packet types we emit. */
#define HDMI_PKT_TYPE_NULL              0x00u
#define HDMI_PKT_TYPE_ACR               0x01u
#define HDMI_PKT_TYPE_AUDIO_SAMPLE      0x02u
#define HDMI_PKT_TYPE_GENERAL_CONTROL   0x03u
#define HDMI_PKT_TYPE_INFOFRAME_AVI     0x82u
#define HDMI_PKT_TYPE_INFOFRAME_AUDIO   0x84u

/*
 * Bit-reversed BCH(64,56) generator polynomial.
 *
 * HDMI 1.4 §5.2.3.5 specifies G(x) = x^8 + x^7 + x^6 + x^4 + x^2 + 1 (0x1D5).
 * Bytes are transmitted LSB-first; in principle the LSB-first feedback mask
 * is the bit-reverse of the lower 8 bits of G(x) — 0xD5 reversed → 0xAB —
 * but the commonly cited value in shipping RP2xxx HDMI implementations
 * (Adam Heinrich's pico_dvi_audio, Wren6991's PicoDVI) is 0x83, and we
 * default to that for compatibility. Override with -DHDMI_BCH_POLY=0xAB at
 * configure time if your receiver prefers the mathematically derived value.
 */
#ifndef HDMI_BCH_POLY
#define HDMI_BCH_POLY 0x83u
#endif

/* TERC4 codeword lookup (4-bit -> 10-bit TMDS). HDMI 1.4 Table 5-6. */
extern const uint16_t hdmi_terc4_lut[16];

/* Compute the BCH ECC byte for a stream of bits (LSB-first within each byte). */
uint8_t hdmi_bch_ecc(const uint8_t *data, uint32_t nbits);

/*
 * Encode a single 32-pixel packet into 32 HSTX RAW words.
 *
 * `first_packet` should be non-zero only for the first packet within a data
 * island (sets the "new packet" indicator on CH0). HSYNC/VSYNC state is
 * embedded in every TERC4 symbol on CH0; pass the line-level state.
 */
void hdmi_di_encode_packet(const HdmiPacket *pkt,
                           int first_packet,
                           uint32_t hsync_active,  /* 1 = sync asserted (low) */
                           uint32_t vsync_active,
                           uint32_t out_words[HDMI_PACKET_RAW_WORDS]);

/*
 * Emit a full data-island block: preamble (8 px) + leading guard band (2 px)
 * + N packets + trailing guard band (2 px). Returns the number of RAW words
 * written. `out_words` must hold at least 12 + 32*npackets entries.
 */
uint32_t hdmi_di_emit_block(const HdmiPacket *packets, uint32_t npackets,
                            uint32_t hsync_active, uint32_t vsync_active,
                            uint32_t *out_words);

/* --- Helpers to build the packets we use. --------------------------------- */

void hdmi_pkt_make_null(HdmiPacket *pkt);

void hdmi_pkt_make_general_control(HdmiPacket *pkt,
                                   int avmute_set, int avmute_clear);

/*
 * Audio Clock Regeneration. For 640x480p @ 25 MHz pixel clock and 48 kHz
 * audio, the canonical N/CTS pair is N=6144, CTS=25000 (we run at 25 MHz
 * exactly, not 25.175). See HDMI 1.4 §7.2.
 */
void hdmi_pkt_make_acr(HdmiPacket *pkt, uint32_t n_value, uint32_t cts_value);

/*
 * AVI InfoFrame for 640x480p, RGB, full range. Version 2.
 */
void hdmi_pkt_make_avi_infoframe(HdmiPacket *pkt);

/*
 * Audio InfoFrame for 2-channel PCM, 48 kHz, 16-bit, refer-to-stream coding.
 */
void hdmi_pkt_make_audio_infoframe(HdmiPacket *pkt);

/*
 * Audio Sample packet carrying up to 4 stereo L/R int16 sample pairs.
 *
 * `nsamples` (0..4) tells the receiver how many subpacket slots are valid.
 * `frame_no` is the IEC-60958 frame number within the audio block (used to
 * mark the "B" preamble every 192 frames); the caller advances it as it
 * pulls samples out of its ring.
 */
void hdmi_pkt_make_audio_sample(HdmiPacket *pkt,
                                const int16_t samples_lr[8],
                                uint32_t nsamples,
                                uint32_t *frame_no);

/* Constants for VBI cmd-list assembly. ----------------------------------- */

/* Pre-built RAW word for a single video-preamble pixel (CH1=CTL01, CH2=CTL00). */
extern const uint32_t hdmi_video_preamble_word;
/* Pre-built RAW word for a single data-island preamble pixel. */
extern const uint32_t hdmi_di_preamble_word;
/* Pre-built RAW words for the two pixels of a video guard band. */
extern const uint32_t hdmi_video_guardband_word;

#endif
