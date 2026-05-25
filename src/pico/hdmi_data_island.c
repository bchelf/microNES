#include "hdmi_data_island.h"

#include <string.h>

/* TMDS control codewords (HDMI 1.4 §5.4.2.2). */
#define TMDS_CTRL_00 0x354u  /* D1=0, D0=0 */
#define TMDS_CTRL_01 0x0ABu  /* D1=0, D0=1 */
#define TMDS_CTRL_10 0x154u  /* D1=1, D0=0 */
#define TMDS_CTRL_11 0x2ABu  /* D1=1, D0=1 */

/*
 * TERC4 codewords. HDMI 1.4 Table 5-6.
 *
 * Index = 4-bit data (bit 0 of input = bit 0 of the TMDS codeword by spec).
 * Output is the 10-bit TMDS codeword to transmit (bit 0 first on the wire).
 */
const uint16_t hdmi_terc4_lut[16] = {
    0x29Cu, 0x263u, 0x2E4u, 0x2E2u,
    0x171u, 0x11Eu, 0x18Eu, 0x13Cu,
    0x2CCu, 0x139u, 0x19Cu, 0x2C6u,
    0x28Eu, 0x271u, 0x163u, 0x2C3u,
};

/*
 * BCH ECC byte for HDMI data island.
 *
 * LSB-first shift register; see HDMI_BCH_POLY in the header for the
 * derivation of the feedback mask.
 */
uint8_t hdmi_bch_ecc(const uint8_t *data, uint32_t nbits) {
    uint8_t ecc = 0u;
    for (uint32_t i = 0u; i < nbits; ++i) {
        uint8_t db = (uint8_t)((data[i >> 3u] >> (i & 7u)) & 1u);
        uint8_t fb = (uint8_t)((ecc ^ db) & 1u);
        ecc = (uint8_t)(ecc >> 1u);
        if (fb) {
            ecc ^= (uint8_t)HDMI_BCH_POLY;
        }
    }
    return ecc;
}

static inline uint32_t pack_terc4(uint8_t ch0, uint8_t ch1, uint8_t ch2) {
    return (uint32_t)hdmi_terc4_lut[ch0 & 0xfu] |
           ((uint32_t)hdmi_terc4_lut[ch1 & 0xfu] << 10) |
           ((uint32_t)hdmi_terc4_lut[ch2 & 0xfu] << 20);
}

static inline uint32_t pack_tmds_ctl(uint16_t ch0, uint16_t ch1, uint16_t ch2) {
    return (uint32_t)ch0 |
           ((uint32_t)ch1 << 10) |
           ((uint32_t)ch2 << 20);
}

/* Pre-baked constant pixel words. */
const uint32_t hdmi_video_preamble_word =
    /* CH0: sync inactive (V=1,H=1), CH1: CTL0=1 CTL1=0, CH2: CTL2=0 CTL3=0. */
    TMDS_CTRL_11 | ((uint32_t)TMDS_CTRL_01 << 10) | ((uint32_t)TMDS_CTRL_00 << 20);

const uint32_t hdmi_di_preamble_word =
    /* CH0: sync inactive, CH1: CTL0=1 CTL1=0, CH2: CTL2=1 CTL3=0. */
    TMDS_CTRL_11 | ((uint32_t)TMDS_CTRL_01 << 10) | ((uint32_t)TMDS_CTRL_01 << 20);

const uint32_t hdmi_video_guardband_word =
    /* CH0 = 0x2CC, CH1 = 0x133, CH2 = 0x2CC. */
    0x2CCu | (0x133u << 10) | (0x2CCu << 20);

/*
 * Data island leading/trailing guard band. CH0 carries TERC4 of
 * {1, 1, VSYNC, HSYNC} = 0xC | (V<<1) | H ; CH1 and CH2 are fixed.
 */
static inline uint32_t di_guardband_word(uint32_t hsync_active, uint32_t vsync_active) {
    /* hsync_active==1 means the sync line is currently driven low (asserted).
     * CH0 sync bits send the raw line state: bit 0 = HSYNC line, bit 1 = VSYNC line.
     * "Sync line low" == "asserted" for active-low syncs; that's the value the
     * receiver samples, so we encode the line-state bits as-is (0 = asserted).
     */
    uint8_t h_bit = (uint8_t)(hsync_active ? 0u : 1u);
    uint8_t v_bit = (uint8_t)(vsync_active ? 0u : 1u);
    uint8_t ch0_terc4 = (uint8_t)(0xCu | (v_bit << 1u) | h_bit);
    return (uint32_t)hdmi_terc4_lut[ch0_terc4] |
           ((uint32_t)0x133u << 10) |
           ((uint32_t)0x133u << 20);
}

void hdmi_di_encode_packet(const HdmiPacket *pkt,
                           int first_packet,
                           uint32_t hsync_active,
                           uint32_t vsync_active,
                           uint32_t out_words[HDMI_PACKET_RAW_WORDS]) {
    /* Build 32-bit header (HB0|HB1|HB2|ECC) and four 64-bit subpackets. */
    uint8_t hdr_bytes[4];
    hdr_bytes[0] = pkt->header[0];
    hdr_bytes[1] = pkt->header[1];
    hdr_bytes[2] = pkt->header[2];
    hdr_bytes[3] = hdmi_bch_ecc(hdr_bytes, 24u);

    uint8_t sp[4][8];
    for (uint32_t s = 0u; s < 4u; ++s) {
        for (uint32_t b = 0u; b < 7u; ++b) {
            sp[s][b] = pkt->subpackets[s][b];
        }
        sp[s][7] = hdmi_bch_ecc(sp[s], 56u);
    }

    uint8_t h_line = (uint8_t)(hsync_active ? 0u : 1u);
    uint8_t v_line = (uint8_t)(vsync_active ? 0u : 1u);

    /* Encode CH0 (header + sync + packet-start flag). */
    uint8_t hv = (uint8_t)(h_line | (v_line << 1u));
    uint8_t hv1 = (uint8_t)(hv | 0x08u);
    if (!first_packet) {
        hv = hv1;
    }

    uint16_t ch0_lane[32];
    uint32_t idx = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        uint8_t h = hdr_bytes[i];
        ch0_lane[idx++] = hdmi_terc4_lut[((h << 2) & 4u) | hv]; hv = hv1;
        ch0_lane[idx++] = hdmi_terc4_lut[((h << 1) & 4u) | hv];
        ch0_lane[idx++] = hdmi_terc4_lut[(h & 4u)        | hv];
        ch0_lane[idx++] = hdmi_terc4_lut[((h >> 1) & 4u) | hv];
        ch0_lane[idx++] = hdmi_terc4_lut[((h >> 2) & 4u) | hv];
        ch0_lane[idx++] = hdmi_terc4_lut[((h >> 3) & 4u) | hv];
        ch0_lane[idx++] = hdmi_terc4_lut[((h >> 4) & 4u) | hv];
        ch0_lane[idx++] = hdmi_terc4_lut[((h >> 5) & 4u) | hv];
    }

    /* Encode CH1/CH2 (subpacket data). Matches pico_hdmi's bit-transpose
     * interleaving: for each byte position across the 4 subpackets, a 4×8
     * matrix transpose puts one bit from each subpacket into contiguous
     * nibbles, which are then assigned to 4 consecutive pixels. */
    uint16_t ch1_lane[32], ch2_lane[32];
    for (uint32_t i = 0u; i < 8u; ++i) {
        uint32_t v = (uint32_t)sp[0][i] |
                     ((uint32_t)sp[1][i] << 8u) |
                     ((uint32_t)sp[2][i] << 16u) |
                     ((uint32_t)sp[3][i] << 24u);
        uint32_t t = (v ^ (v >> 7u)) & 0x00AA00AAu;
        v = v ^ t ^ (t << 7u);
        t = (v ^ (v >> 14u)) & 0x0000CCCCu;
        v = v ^ t ^ (t << 14u);
        ch1_lane[i * 4u + 0u] = hdmi_terc4_lut[(v >>  0u) & 0xFu];
        ch1_lane[i * 4u + 1u] = hdmi_terc4_lut[(v >> 16u) & 0xFu];
        ch1_lane[i * 4u + 2u] = hdmi_terc4_lut[(v >>  4u) & 0xFu];
        ch1_lane[i * 4u + 3u] = hdmi_terc4_lut[(v >> 20u) & 0xFu];
        ch2_lane[i * 4u + 0u] = hdmi_terc4_lut[(v >>  8u) & 0xFu];
        ch2_lane[i * 4u + 1u] = hdmi_terc4_lut[(v >> 24u) & 0xFu];
        ch2_lane[i * 4u + 2u] = hdmi_terc4_lut[(v >> 12u) & 0xFu];
        ch2_lane[i * 4u + 3u] = hdmi_terc4_lut[(v >> 28u) & 0xFu];
    }

    for (uint32_t k = 0u; k < HDMI_PACKET_RAW_WORDS; ++k) {
        out_words[k] = (uint32_t)ch0_lane[k] |
                       ((uint32_t)ch1_lane[k] << 10u) |
                       ((uint32_t)ch2_lane[k] << 20u);
    }
}

uint32_t hdmi_di_emit_block(const HdmiPacket *packets, uint32_t npackets,
                            uint32_t hsync_active, uint32_t vsync_active,
                            uint32_t *out) {
    uint32_t *p = out;

    uint16_t ch0;
    if (vsync_active && hsync_active) ch0 = TMDS_CTRL_00;
    else if (vsync_active)            ch0 = TMDS_CTRL_01;
    else if (hsync_active)            ch0 = TMDS_CTRL_10;
    else                              ch0 = TMDS_CTRL_11;
    uint32_t preamble_word = pack_tmds_ctl(ch0, TMDS_CTRL_01, TMDS_CTRL_01);

    for (uint32_t i = 0u; i < HDMI_DI_PREAMBLE_PIXELS; ++i) {
        *p++ = preamble_word;
    }

    uint32_t gb = di_guardband_word(hsync_active, vsync_active);
    *p++ = gb;
    *p++ = gb;

    for (uint32_t i = 0u; i < npackets; ++i) {
        hdmi_di_encode_packet(&packets[i], i == 0u, hsync_active, vsync_active, p);
        p += HDMI_PACKET_RAW_WORDS;
    }

    *p++ = gb;
    *p++ = gb;

    return (uint32_t)(p - out);
}

/* --- Packet builders ----------------------------------------------------- */

void hdmi_pkt_make_null(HdmiPacket *pkt) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->header[0] = HDMI_PKT_TYPE_NULL;
}

void hdmi_pkt_make_general_control(HdmiPacket *pkt,
                                   int avmute_set, int avmute_clear) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->header[0] = HDMI_PKT_TYPE_GENERAL_CONTROL;

    uint8_t sb0 = (uint8_t)((avmute_set ? 0x01u : 0u) |
                             (avmute_clear ? 0x10u : 0u));
    uint8_t sb1 = 0x04u; /* CD=4 → 24 bits per pixel (8 bpc) */

    for (uint32_t s = 0u; s < 4u; ++s) {
        pkt->subpackets[s][0] = sb0;
        pkt->subpackets[s][1] = sb1;
    }
}

void hdmi_pkt_make_acr(HdmiPacket *pkt, uint32_t n_value, uint32_t cts_value) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->header[0] = HDMI_PKT_TYPE_ACR;
    /* HDMI 1.4 §5.3.4: header bytes HB1/HB2 are zero for ACR. */

    /* Each of the 4 subpackets carries the same CTS/N pair. */
    for (uint32_t s = 0u; s < 4u; ++s) {
        uint8_t *sb = pkt->subpackets[s];
        sb[0] = 0u;
        sb[1] = (uint8_t)((cts_value >> 16) & 0x0Fu);
        sb[2] = (uint8_t)((cts_value >> 8) & 0xFFu);
        sb[3] = (uint8_t)(cts_value & 0xFFu);
        sb[4] = (uint8_t)((n_value >> 16) & 0x0Fu);
        sb[5] = (uint8_t)((n_value >> 8) & 0xFFu);
        sb[6] = (uint8_t)(n_value & 0xFFu);
    }
}

/*
 * Sums the bytes of an InfoFrame body and the type/version/length header,
 * returning the checksum byte (two's complement so sum mod 256 == 0).
 */
static uint8_t infoframe_checksum(uint8_t type, uint8_t version, uint8_t length,
                                  const uint8_t *body) {
    uint32_t sum = (uint32_t)type + version + length;
    for (uint32_t i = 0u; i < length; ++i) {
        sum += body[i];
    }
    return (uint8_t)(0x100u - (sum & 0xFFu));
}

/*
 * Pack an InfoFrame body into the four subpackets.
 *
 * HDMI layout (CEA-861): subpacket 0 byte 0 = checksum, subpacket 0 bytes 1..6
 * = body bytes 0..5; subpacket 1 = body bytes 6..12; subpacket 2 = body 13..19;
 * subpacket 3 = body 20..26. Total body capacity = 27 bytes; CEA-861 caps most
 * InfoFrames well below that.
 */
static void pack_infoframe(HdmiPacket *pkt,
                           uint8_t type, uint8_t version, uint8_t length,
                           const uint8_t *body) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->header[0] = type;
    pkt->header[1] = version;
    pkt->header[2] = (uint8_t)(length & 0x1Fu);

    pkt->subpackets[0][0] = infoframe_checksum(type, version, length, body);
    uint32_t bi = 0u;
    for (uint32_t b = 1u; b < 7u && bi < length; ++b, ++bi) {
        pkt->subpackets[0][b] = body[bi];
    }
    for (uint32_t s = 1u; s < 4u; ++s) {
        for (uint32_t b = 0u; b < 7u && bi < length; ++b, ++bi) {
            pkt->subpackets[s][b] = body[bi];
        }
    }
}

void hdmi_pkt_make_avi_infoframe(HdmiPacket *pkt) {
    /* AVI InfoFrame version 2, body length 13 (CEA-861-F §6.4). */
    uint8_t body[13];
    memset(body, 0, sizeof(body));
    /* Byte 1: Y[6:5]=0 (RGB), A0[4]=0, B[3:2]=0, S[1:0]=0. */
    body[0] = 0x00u;
    /* Byte 2: C[7:6]=0 (no data), M[5:4]=0 (no aspect ratio), R[3:0]=8
     * (same as picture). VIC=0 below tells the sink "no CEA mode claimed,"
     * so the aspect-ratio M field must also be 0 (per CEA-861). */
    body[1] = (uint8_t)((0u << 6) | (0u << 4) | 8u);
    /* Byte 3: ITC[7]=0, EC[6:4]=0, Q[3:2]=2 (RGB full range), SC[1:0]=0. */
    body[2] = (uint8_t)(2u << 2);
    /* Byte 4: VIC = 0 — let the sink auto-detect from timing.
     *
     * We could claim VIC=1 (640x480p59.94, 25.175 MHz exact pixel clock) but
     * our HSTX is running 25.0 MHz exactly (clk_sys/2/5). The 0.7%
     * discrepancy is enough for some sinks to reject the InfoFrame and
     * drop the link. VIC=0 sidesteps the issue. */
    body[3] = 0u;
    /* Byte 5..13: pixel repetition and bar info zero. */
    pack_infoframe(pkt, HDMI_PKT_TYPE_INFOFRAME_AVI, 2u, 13u, body);
}

void hdmi_pkt_make_audio_infoframe(HdmiPacket *pkt) {
    /* Audio InfoFrame version 1, body length 10 (CEA-861-F §6.6).
     * Matches pico_hdmi: explicit CT/SF/SS instead of "refer to stream." */
    uint8_t body[10];
    memset(body, 0, sizeof(body));
    /* Byte 1: CT[7:4]=1 (PCM), CC[2:0]=1 (2 channels). */
    body[0] = (uint8_t)(0x01u | (0x01u << 4));
    /* Byte 2: SF[4:2]=3 (48 kHz), SS[1:0]=1 (16 bit). */
    body[1] = (uint8_t)(0x01u | (0x03u << 2));
    /* Byte 3..10: zero. */
    pack_infoframe(pkt, HDMI_PKT_TYPE_INFOFRAME_AUDIO, 1u, 10u, body);
}

/*
 * IEC-60958 consumer channel status (192 bits = 24 bytes).
 * We only set the fields required for LPCM 48 kHz 16-bit stereo;
 * the rest are zero (per IEC-60958-3 §2.2).
 *
 *   Byte 0: bit 0 = 0 (consumer), bit 1 = 0 (LPCM), bits 2..7 = 0.
 *   Byte 1: category code = 0 (general).
 *   Byte 2: source/channel number = 0.
 *   Byte 3: bits 0..3 = sampling frequency. 0010 = 48 kHz.
 *           bits 4..5 = clock accuracy = 00 (level II).
 *   Byte 4: bits 0..3 = word length. 0010 = 16 bits (max 20 bit).
 *           bits 4..7 = original sampling frequency = 0000.
 */
static const uint8_t k_iec60958_channel_status[24] = {
    0x00u, 0x00u, 0x00u, 0x02u, 0x02u,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static inline uint8_t iec60958_channel_status_bit(uint32_t frame_no) {
    uint32_t byte_idx = (frame_no % 192u) / 8u;
    uint32_t bit_idx  = (frame_no % 192u) % 8u;
    return (uint8_t)((k_iec60958_channel_status[byte_idx] >> bit_idx) & 1u);
}

void hdmi_pkt_make_audio_sample(HdmiPacket *pkt,
                                const int16_t samples_lr[8],
                                uint32_t nsamples,
                                uint32_t *frame_no) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->header[0] = HDMI_PKT_TYPE_AUDIO_SAMPLE;

    uint8_t hb1 = 0u;
    for (uint32_t s = 0u; s < nsamples && s < 4u; ++s) {
        hb1 |= (uint8_t)(1u << s);
    }
    pkt->header[1] = hb1;

    uint8_t hb2 = 0u;
    for (uint32_t s = 0u; s < nsamples && s < 4u; ++s) {
        if (((*frame_no + s) % 192u) == 0u) {
            hb2 |= (uint8_t)(1u << (4u + s));
        }
    }
    pkt->header[2] = hb2;

    for (uint32_t s = 0u; s < nsamples && s < 4u; ++s) {
        int16_t l = samples_lr[s * 2u + 0u];
        int16_t r = samples_lr[s * 2u + 1u];
        uint8_t *sb = pkt->subpackets[s];

        sb[0] = 0u;
        sb[1] = (uint8_t)(l & 0xFFu);
        sb[2] = (uint8_t)((l >> 8) & 0xFFu);

        sb[3] = 0u;
        sb[4] = (uint8_t)(r & 0xFFu);
        sb[5] = (uint8_t)((r >> 8) & 0xFFu);

        /* Byte 6: V, U, C, P per channel.
         * Match pico_hdmi: V=U=C=0; parity over data bytes only. */
        uint8_t parity_l = 0u, parity_r = 0u;
        for (uint32_t i = 0u; i < 3u; ++i) {
            uint8_t bl = sb[i];
            uint8_t br = sb[3u + i];
            for (uint32_t b = 0u; b < 8u; ++b) {
                parity_l ^= (uint8_t)((bl >> b) & 1u);
                parity_r ^= (uint8_t)((br >> b) & 1u);
            }
        }
        sb[6] = (uint8_t)((parity_l << 3) | (parity_r << 7));
    }
    *frame_no = *frame_no + nsamples;
}
