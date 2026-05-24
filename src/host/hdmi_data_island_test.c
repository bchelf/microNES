/*
 * hdmi_data_island_test — sanity tests for the portable HDMI data-island
 * encoder. Builds on the host as part of the host platform; intended to be
 * extended with known-good HDMI test vectors during hardware bring-up.
 */

#include "../pico/hdmi_data_island.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures = 0;

static void expect_eq_u32(const char *label, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got 0x%08x want 0x%08x\n",
                label, (unsigned)got, (unsigned)want);
        ++s_failures;
    }
}

static void expect_eq_u8(const char *label, uint8_t got, uint8_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got 0x%02x want 0x%02x\n", label, got, want);
        ++s_failures;
    }
}

static void test_terc4_table(void) {
    /* Spot-check a couple of entries from HDMI 1.4 Table 5-6. */
    expect_eq_u32("terc4[0x0]", hdmi_terc4_lut[0x0], 0x29Cu);
    expect_eq_u32("terc4[0x8]", hdmi_terc4_lut[0x8], 0x2CCu);
    expect_eq_u32("terc4[0xF]", hdmi_terc4_lut[0xF], 0x2C3u);
}

static void test_bch_self_consistent(void) {
    /*
     * We do not yet have a vetted external HDMI BCH test vector embedded
     * here, so we sanity-check structural properties:
     *   - ECC of all-zero input is zero.
     *   - ECC depends on input (different inputs produce different ECC).
     *   - The 24-bit and 56-bit forms both terminate without crashing.
     */
    uint8_t zeros[16];
    memset(zeros, 0, sizeof(zeros));
    expect_eq_u8("bch zero(24)", hdmi_bch_ecc(zeros, 24u), 0u);
    expect_eq_u8("bch zero(56)", hdmi_bch_ecc(zeros, 56u), 0u);

    uint8_t a[3] = {0x82u, 0x02u, 0x0Du};
    uint8_t b[3] = {0x84u, 0x01u, 0x0Au};
    uint8_t ea = hdmi_bch_ecc(a, 24u);
    uint8_t eb = hdmi_bch_ecc(b, 24u);
    if (ea == eb) {
        fprintf(stderr, "FAIL: bch ECC collapses for two distinct AVI headers\n");
        ++s_failures;
    }
    /* Report the computed values so we can compare to a sniffer trace. */
    fprintf(stdout, "info: bch ECC(AVI hdr 0x82 0x02 0x0D) = 0x%02x\n", ea);
    fprintf(stdout, "info: bch ECC(Audio hdr 0x84 0x01 0x0A) = 0x%02x\n", eb);
    fprintf(stdout, "info: bch poly constant = 0x%02x\n", (unsigned)HDMI_BCH_POLY);
}

static void test_packet_encode_size(void) {
    /*
     * Encoding a packet must produce exactly 32 RAW words and the first word
     * must have the "new packet" indicator set on CH0.
     */
    HdmiPacket pkt;
    hdmi_pkt_make_avi_infoframe(&pkt);
    uint32_t words[32];
    hdmi_di_encode_packet(&pkt, 1, 0u, 0u, words);

    /*
     * CH0 of pixel 0 must have bit 3 (new packet) set when first_packet=1.
     * Bit 3 of the 4-bit TERC4 input means CH0 TERC4 input has 0x8 OR'd in.
     * We can't easily decode the TMDS output back to the 4-bit input, but we
     * can check that the encoding differs from a non-first-packet encoding.
     */
    uint32_t words2[32];
    hdmi_di_encode_packet(&pkt, 0, 0u, 0u, words2);
    if (words[0] == words2[0]) {
        fprintf(stderr, "FAIL: new-packet indicator not reflected in pixel 0\n");
        ++s_failures;
    }
    for (uint32_t k = 1u; k < 32u; ++k) {
        if (words[k] != words2[k]) {
            fprintf(stderr, "FAIL: new-packet indicator leaked past pixel 0 at k=%u\n", k);
            ++s_failures;
            break;
        }
    }
}

static void test_block_word_count(void) {
    HdmiPacket pkts[3];
    hdmi_pkt_make_avi_infoframe(&pkts[0]);
    hdmi_pkt_make_audio_infoframe(&pkts[1]);
    hdmi_pkt_make_general_control(&pkts[2], 0, 1);
    uint32_t buf[2 + 32 * 3 + 2]; /* guard + 3 packets + guard (no preamble) */
    uint32_t n = hdmi_di_emit_island_body(pkts, 3u, 0u, 0u, buf);
    if (n != (uint32_t)sizeof(buf) / 4u) {
        fprintf(stderr, "FAIL: block size = %u, expected %u\n",
                n, (unsigned)(sizeof(buf) / 4u));
        ++s_failures;
    }
}

static void test_audio_sample_packet(void) {
    HdmiPacket pkt;
    int16_t lr[8] = { 100, -200, 300, -400, 500, -600, 700, -800 };
    uint32_t frame_no = 0;
    hdmi_pkt_make_audio_sample(&pkt, lr, 4u, &frame_no);

    expect_eq_u8("ASP HB0", pkt.header[0], HDMI_PKT_TYPE_AUDIO_SAMPLE);
    expect_eq_u8("ASP sample_present", pkt.header[1], 0x0Fu);
    /* First call: B bit set on the first subpacket only. */
    if ((pkt.header[2] & 0x10u) == 0u) {
        fprintf(stderr, "FAIL: ASP block-start B bit not set on first frame\n");
        ++s_failures;
    }
    /* Left channel sample bytes for pair 0 should encode int16 LE in bytes 1..2. */
    expect_eq_u8("ASP sp0 lo lsb", pkt.subpackets[0][1], 100u & 0xFFu);
    expect_eq_u8("ASP sp0 lo msb", pkt.subpackets[0][2], (uint8_t)((100 >> 8) & 0xFF));

    if (frame_no != 4u) {
        fprintf(stderr, "FAIL: ASP frame counter advanced by %u, expected 4\n",
                (unsigned)frame_no);
        ++s_failures;
    }
}

int main(void) {
    test_terc4_table();
    test_bch_self_consistent();
    test_packet_encode_size();
    test_block_word_count();
    test_audio_sample_packet();

    if (s_failures) {
        fprintf(stderr, "FAILED: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("OK: hdmi_data_island_test\n");
    return 0;
}
