# CLAUDE.md — Agent session notes

This file records fixes and findings from Claude Code agent sessions.

## Recent fixes

### APU DMC emulation improvements (branch: claude/fix-apu-bugs-rWRjE)

**Goal**: Improve AccuracyCoin pages 13 and 14 pass rates.

**Changes made**:

1. **DMC rate table fix** (`apu_micrones.c`): Fixed the DMC timer period
   table to use correct CPU-cycle values from NESdev.  The `8-dmc_rates`
   AccuracyCoin test went from FAIL to PASS.

2. **DMC DMA sample buffer** (`apu_micrones.c`, `apu_micrones.h`, `nes.h`,
   `nes.c`): Implemented actual DMA fetches from CPU memory into the DMC
   sample buffer.
   - Added `dmc_dma_needed` flag to `ApuDmcChannel`.
   - Added `nes_dmc_dma_if_needed()` inline in `nes.h`; called after each
     instruction (step_instruction path) and before/after `apu_step` in the
     scanline path.
   - DMC now reads successive bytes from `dmc_current_addr` (wraps
     $FFFF→$8000), fills the sample buffer, and requests the next DMA when
     the shift register empties with more bytes remaining.
   - DMC stalls (stops firing) while waiting for DMA to fill the buffer.

3. **DMC delta output** (`apu_micrones.c`): The shift register now actually
   shifts bits out and applies the ±2 delta to `dmc_output_level` (clamped
   0–127) on each DMC timer fire.

4. **Scanline path DMC DMA** (`nes.c`): Added `nes_dmc_dma_if_needed` calls
   to `nes_step_scanline` (before and after `apu_step`) so embedded/Pico
   frontends that use the scanline-granularity stepper also service DMC DMA.

**AccuracyCoin page 14 results after fixes**:

| Test | Before | After |
|------|--------|-------|
| LENGTH COUNTER | PASS | PASS |
| LENGTH TABLE | PASS | PASS |
| FRAME COUNTER IRQ | FAIL 6 | FAIL 6 |
| FRAME COUNTER 4-STEP | PASS | PASS |
| FRAME COUNTER 5-STEP | PASS | PASS |
| DELTA MODULATION CHANNEL | FAIL 1 | FAIL I (test 18) |
| APU REGISTER ACTIVATION | FAIL 1 | FAIL 1 |
| CONTROLLER STROBING | FAIL 3 | FAIL 3 |
| CONTROLLER CLOCKING | FAIL 6 | FAIL 6 |

The DMC test now passes sub-tests 1 through H (17 sub-tests).  Sub-test I
("1-byte sample ends immediately after start") and beyond require
cycle-accurate per-instruction DMA stall modeling that is out of scope for
the current instruction-granularity emulator.

**Known remaining gaps** (require architectural changes):

- DMC sub-test I and beyond: need cycle-accurate DMA stalling (CPU halted
  during DMA for 1–4 cycles).
- FRAME COUNTER IRQ FAIL 6: needs put/get CPU bus cycle parity tracking.
- APU REGISTER ACTIVATION FAIL 1: needs open-bus emulation and cycle-
  accurate DMA timing.
- CONTROLLER STROBING/CLOCKING FAIL: need put/get cycle parity for
  read-modify-write strobe detection.
- Page 13 DMA tests: all require cycle-accurate DMA halt/alignment
  cycle emulation.
