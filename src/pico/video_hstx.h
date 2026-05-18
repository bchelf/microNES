#ifndef MICRONES_VIDEO_HSTX_H
#define MICRONES_VIDEO_HSTX_H

/*
 * video_hstx — RP2350 HSTX-driven DVI/HDMI output for micrones.
 *
 * Mode: 640x480 @ 60 Hz (CEA-861 / VESA DMT)
 *   pixel clock 25.200 MHz, TMDS bit-rate 252.0 MHz/lane
 *   sys_clk      = 252 MHz   (PLL VCO 1260 / div 5,1; same VCO as 315 MHz analog)
 *   HSTX clk     = sys_clk / 2 = 126 MHz
 *   serial rate  = HSTX clk × 2 (DDR, csr.shift = 2 bits/cycle) = 252 Mbps/lane
 *
 * Pin map (HSTX is hard-wired to GP12..GP19 on RP2350).  The polarity
 * assignment matches the microNES v0.1 PCB layout, where the LOWER-
 * numbered GP in each pair drives the negative side of the differential:
 *
 *   GP12 — TMDS clock-        |  HDMI pin 12  (CK-)
 *   GP13 — TMDS clock+        |  HDMI pin 10  (CK+)
 *   GP14 — TMDS data 0 (B)-   |  HDMI pin 9
 *   GP15 — TMDS data 0 (B)+   |  HDMI pin 7
 *   GP16 — TMDS data 1 (G)-   |  HDMI pin 6
 *   GP17 — TMDS data 1 (G)+   |  HDMI pin 4
 *   GP18 — TMDS data 2 (R)-   |  HDMI pin 3
 *   GP19 — TMDS data 2 (R)+   |  HDMI pin 1
 *
 * Each pair has 270 Ω series resistors on-board for TMDS-compliant
 * source termination.
 *
 * Frame source: micrones renders the NES picture into its 256x240 indexed
 * framebuffer.  This module owns:
 *   - the NES palette → RGB565 lookup table
 *   - two RGB565 line buffers (1280 bytes each) for the active 640-pixel scanout
 *   - the HSTX command lists (per-line script of HSYNC/blanking/active-pixel
 *     transfers)
 *   - the DMA chain that feeds HSTX's FIFO without any per-line CPU work
 *
 * Performance contract:
 *   - HSTX serialises pixels in hardware; DMA scans line buffers without CPU
 *     involvement.
 *   - Per-line refill (NES indexed → RGB565, with 2x horizontal duplication
 *     and pillarboxing) runs from a DMA-completion interrupt.  A 256-pixel
 *     source row takes ~1–2 µs at 252 MHz; 480 visible lines costs ~1 ms /
 *     frame, which is well under the 16.6 ms budget and does not affect the
 *     emulator's 60 fps.
 *   - The visible image is 512x480 (NES 256x240 scaled 2x in both axes),
 *     pillar-boxed inside the 640x480 frame (64 black pixels each side).
 */

#include "framebuffer.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t frames_presented;
    uint64_t lines_filled;
    uint64_t fill_us_total;
    uint64_t fill_us_max_per_line;
    uint32_t late_fills;        /* line ISR ran after DMA had already begun next line */
} VideoHstxStats;

/*
 * Initialise the HSTX peripheral, build the static command lists, configure
 * GP12..GP19 for HSTX function, claim DMA channels, prime line buffers, and
 * start the continuous DMA chain.  Returns false on failure with a message
 * available via video_hstx_last_error().
 *
 * After this call the link is "live" — even before the emulator presents
 * any frame the link is sending pillar-boxed black at 60 Hz, which is what
 * monitors expect (DDC/EDID handshake is HDMI-only and is not driven here;
 * DVI mode does not require it).
 */
bool video_hstx_init(void);

const char *video_hstx_last_error(void);

/*
 * Present a complete NES indexed framebuffer.  Non-blocking: copies the
 * 256x240 indexed bytes into an internal "present" buffer that the line-fill
 * ISR reads from.  The next visible scanline that fires from the active area
 * will pick up the new pixels.  No tearing-control is attempted; at 60 Hz
 * NES vs 60 Hz HDMI the visible tear is fixed near the top of the frame.
 *
 * Performance: ~50 µs (256*240 = 61,440 byte memcpy at 252 MHz).
 */
void video_hstx_present_frame(const NesFrameBuffer *frame);

/*
 * Test pattern for bring-up: vertical color bars rendered into the present
 * buffer.  Lets you confirm the link is alive before wiring the emulator.
 */
void video_hstx_draw_test_pattern(void);

void video_hstx_get_stats(VideoHstxStats *stats_out);

#endif /* MICRONES_VIDEO_HSTX_H */
