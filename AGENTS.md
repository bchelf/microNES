# micrones Agent Notes

This file summarizes the current state of `micrones`, what has already been implemented and validated, what has been learned during debugging, and what remains.

## Project Intent

`micrones` is intentionally narrow:

- It is not a general NES emulator, yet, but eventually will become one.
- It targets the original Super Mario Bros ROM.
- It only supports mapper 0 / NROM, but other support is in the works in a branch called ben/make-celeste-work
- It is being developed as a shared portable NES runtime plus thin host and Pico frontends.
- The long-term embedded target is RP2350 / Raspberry Pi Pico 2.

The current architecture is:

- `src/common/`
  - Portable shared emulator/runtime code.
  - No SDL, no Pico SDK, no host-only image/audio/window code.
- `src/host/`
  - Host-only tooling:
  - smoke validation runner
  - PNG export
  - ffmpeg video capture
  - SDL interactive runner
  - SDL audio playback
- `src/pico/`
  - Pico firmware and hardware-specific code.
- `src/esp32s3`
  - ESP32s3 firmware for OLED 1.9" display (and external speaker)

## What Is Working

### Build / Targets (OUT OF DATE - NEEDS TO BE UPDATED)

- Host smoke target builds and runs:
  - `build-host/micrones_smoke`
- Host SDL interactive runner builds and runs:
  - `build-host/micrones_run`
- Pico firmware builds and produces:
  - `build-pico/micrones.uf2`
  - `build-pico/micrones.elf`

### Core Emulator

- iNES ROM parsing works.
- Mapper 0 / NROM-only support works.
- CPU execution is deterministic for SMB1.
- Missing official CPU opcodes needed by SMB1 have already been implemented.
- No illegal opcodes have been required by SMB1 so far.
- Deterministic host validation has repeatedly passed.

### PPU / Video

- Background rendering exists and produces a visible `256x240` framebuffer.
- Minimal sprite composition exists.
- Sprite 0 hit is implemented from real OAM/background overlap.
- SMB1 gets past the early and later sprite-0-hit wait loops.
- The later sprite-0-hit failure was traced to background address derivation and fixed with a more honest latched `v/t/x/w`-style model.
- A later visual split bug near the HUD/title-card boundary was traced to deferred whole-scanline rendering using stale scanline-start scroll state when SMB updated `PPUSCROLL` on visible scanline 32.
- That split bug was fixed by refreshing the deferred scanline render latch when visible writes hit scroll-affecting PPU registers.

### Host Tooling

- PNG export works from the host runner.
- ffmpeg video capture works by piping raw grayscale frames into ffmpeg.
- SDL interactive window works.
- Keyboard input mapping works for SMB1 gameplay.
- Host-side approximate color mapping exists for SDL display.
- Host-side frame pacing infrastructure exists.

### Audio

- Shared-core PCM sample pipeline exists in `src/common`.
- Host-side SDL audio playback works.
- SMB1 audio is audible and recognizable.
- Audio is still incomplete and simplified.

## Current Architecture Details

### Shared Core

Key portable areas include:

- CPU
- PPU
- APU
- cartridge/NROM
- input/controller state
- framebuffer and scanline types
- frame pacing abstraction

Important design rule:

- Keep emulator logic portable.
- Do not add host-only or Pico-only dependencies to `src/common/`.

### Host Side

Current host capabilities:

- smoke validation with deterministic reporting
- frame hashing and state hashing
- sprite-0 diagnostics
- PNG export
- ffmpeg video capture
- SDL interactive window
- SDL keyboard controller input
- SDL audio playback

### Pico Side

Current Pico scope:

- buildable firmware
- earlier composite bring-up code still exists
- Pico-specific timing shim exists for future shared pacing integration

Do not assume the Pico runtime is yet using the full host-side emulator/display/audio path.

## Important Findings So Far

### 1. SMB1 CPU Execution Was Not the Main Blocker

Earlier debugging showed SMB1 could run millions of instructions deterministically. Once the missing official opcodes needed by SMB1 were implemented, the next blockers were PPU-side, not CPU decode.

### 2. The First Major Gameplay Stall Was Sprite-0-Hit Related

SMB1 stalled in the loop at:

- `LDA $2002`
- `AND #$40`
- `BEQ ...`

This was because `PPUSTATUS` bit 6 was never being raised. The minimal viable sprite-0-hit path fixed that.

### 3. The Later Gameplay Stall Was Also Not a Sprite-OAM Problem

Forensic work showed that later in gameplay:

- sprite 0 OAM remained plausible
- sprite 0 still had nonzero pixels
- sprite 0 still appeared in first-8 scanline selection
- the actual failure was that background under sprite 0 became transparent in the current model

This pointed to background/scroll/address derivation, not sprite corruption.

### 4. A More Honest Latched Scroll/Address Model Was Required

The simplified global scroll model was not enough. Moving to a more honest `PPUCTRL` / `PPUSCROLL` / `PPUADDR`-driven latched address model was required to keep later sprite-0 hit behavior working.

### 5. The HUD / Title-Card Visual Artifact Was a Same-Scanline Split Bug

The artifact became visually obvious around 8-10 seconds into gameplay.

Observed symptom:

- a line at the top of the title card persisted instead of scrolling with the card
- this was visible in PNG/video captures, so not an SDL presentation issue

What we learned:

- the bad row was scanline `32`
- SMB writes `PPUSCROLL` during visible scanline `32`
- the renderer defers whole-scanline drawing until end-of-line
- the scanline renderer had been using stale line-start render state for that row

Minimal fix:

- when visible writes hit `PPUCTRL`, `PPUSCROLL`, or `PPUADDR`, refresh the deferred scanline render latch for that same line

This preserved the scanline-based renderer without a broad PPU rewrite.

## Determinism / Validation Status

Validated repeatedly:

- host smoke determinism passes
- frame hashes are stable under repeat runs
- sprite-0 hit behavior is deterministic
- host and Pico builds both succeed after recent changes

Useful smoke commands:

```sh
cd /Users/bchelf/microNES
cmake --build build-host -j
./build-host/micrones_smoke roms/smb1.nes
```

Frame dump examples:

```sh
./build-host/micrones_smoke roms/smb1.nes 5000000 /tmp/micrones_8s.png
./build-host/micrones_smoke roms/smb1.nes 6200000 /tmp/micrones_10s.png
```

Video capture example:

```sh
./build-host/micrones_smoke roms/smb1.nes --steps 17670500 --video-out build-host/smb_30s.mp4
```

SDL runner:

```sh
./build-host/micrones_run roms/smb1.nes
```

## Current Audio Status

Audio is working end-to-end, but it is still incomplete.

What is true right now:

- audible SMB1 audio exists
- it is recognizable and in sync
- it is not badly delayed or grossly distorted

Known limitations:

- bass is still weaker than it should be
- some sound effects still sound abbreviated or clipped at the tail
- APU behavior is still approximate
- noise/DMC/precise envelope/frame-counter behavior are not yet complete enough for faithful SMB audio

Conclusion:

- the SDL host audio backend is probably not the main problem now
- the remaining work is primarily in `src/common/apu.*`

## Current Rendering Status

What works:

- background rendering is solid enough for SMB1 progression and inspection
- sprite composition is present and sufficient for recognizable gameplay
- sprite-0 hit works in early and later gameplay
- the major late split bug at the HUD/title-card boundary has been fixed narrowly

What is still simplified:

- no cycle-accurate PPU fetch pipeline
- no exact secondary OAM behavior
- no sprite overflow behavior
- no full palette-accurate NES color in the core
- host color display is still an approximation layered on top of the debug-oriented framebuffer

## Current Host SDL Status

Interactive runner status:

- window rendering works
- keyboard input works
- gameplay works
- grayscale upload bug was fixed by using the correct SDL pixel format for byte-wise RGBA data
- approximate host-only color mapping exists
- frame pacing exists but can still look choppy or tear depending on mode

Likely causes of choppiness/tearing already identified:

- pacer-driven presentation with vsync disabled
- host scheduling jitter
- refresh mismatch between NES cadence and actual display

Practical modes:

- live play: prefer `--vsync`
- benchmarking/perf: prefer `--no-vsync --unthrottled`

## What Remains

### APU

Highest-value remaining audio work:

- improve frame sequencer timing
- improve envelope behavior
- improve length counter behavior
- improve triangle sustain / presence
- improve pulse/noise completeness

### PPU

Still intentionally incomplete:

- no full cycle-accurate PPU
- no exact per-dot reload/fetch behavior
- no sprite overflow emulation
- no exact secondary OAM behavior
- no exact final NES palette behavior in the core

### Pico Runtime Integration

Still to do later:

- connect the shared emulator core to a Pico-ready video path
- connect the shared PCM sample pipeline to a Pico audio backend
- integrate the shared frame pacer into the eventual Pico runtime loop
- performance work for RP2350

### General Project Direction

The next work should stay disciplined:

- avoid broad emulator-generalization
- continue prioritizing SMB1-specific correctness
- keep portability boundaries intact

## Pico I2S Audio Implementation (MAX98357)

### Target

`micrones_pico_tft_max98357` uses a MAX98357A I2S amplifier connected to:

- GP10 — BCLK (PIO side-set)
- GP11 — DIN (PIO out pin 0)
- GP12 — LRCLK/WS (PIO out pin 1)

### PIO Program

`audio_i2s_max98357.pio` drives a standard 64-BCLK stereo I2S frame:
- 2 output pins: DIN (base) and LRCLK (base+1) via `out pins, 2`
- BCLK driven by side-set
- Each stereo frame = 133 SM cycles (`set y` + 2×(`set x` + 32×(`out`+`jmp`) + `jmp y--`))
- Data changes on falling BCLK edge, sampled by MAX98357 on rising edge ✓

### Encoding (`audio_i2s_encode_sample`)

Each mono sample is duplicated to both channels. Each channel = 32 BCLK:
- 1 dummy bit (WS=0 left, WS=1 right) — this is the "1 BCLK before MSB" required by standard I2S
- 16 data bits, MSB first
- 15 zero-pad bits

The 2-bit symbol layout packed into each 32-bit word:
- Bit [31] of word → LRCLK (WS) — higher out pin (base+1 = GP12)
- Bit [30] of word → DIN — lower out pin (base = GP11)
- First symbol occupies bits [31:30], last symbol occupies bits [1:0]

### Critical: PIO Shift Direction

**`sm_config_set_out_shift(c, shift_right, autopull, threshold)`**

- `shift_right = false` → shift OSR **left** → emits bits [31:30] first → **MSB first** ✓
- `shift_right = true` → shift OSR right → emits bits [1:0] first → **LSB first** ✗

The parameter is named `shift_right` in the pico-sdk source. For I2S (MSB first with first symbol in bits [31:30]), always use **`false`**. Mistaking this for "left=false" vs "right=true" labeling previously caused the bit stream to be reversed, producing distorted crackle instead of audio. Confirm in pico-sdk: `pio.h` documents `true` as "shift OSR to right."

### Ring Buffer and Rate Mismatch

The NES APU produces ~799 samples/NES frame. At the 60.0 fps wall-clock cap (vs the NES native ~60.099 Hz), production is ~47,940 samples/sec while I2S consumes 48,000/sec. In practice the buffer can drift a few samples per frame due to measurement timing.

When the ring fills, the old policy (drop new samples) caused silence: the DMA would drain all 4096 stale samples before new audio could resume.

**Fix:** circular overwrite in `push_samples` — when full, evict the oldest sample with an IRQ-safe head advance:

```c
uint32_t save = save_and_disable_interrupts();
s_pcm_head = (s_pcm_head + 1u) & (AUDIO_PCM_RING_SIZE - 1u);
restore_interrupts(save);
```

The IRQ disable is required because the DMA ISR (`audio_i2s_fill_dma_block`) also modifies `s_pcm_head`; a torn read-modify-write from the main thread would corrupt the pointer. The critical section is ~4 instructions.

### Diagnostic Fields (audio diag: printf, every 60 frames)

- `underruns=N` — DMA found empty buffer; should stabilize near 0 in steady state
- `overruns=N` — push_samples evicted an old sample; a small steady nonzero rate is normal
- `buf_level=N` — current ring fill (0–4095); healthy range is roughly 500–2000
- `nonzero=1` — APU is producing non-silent samples (0 means APU muted or not stepping)
- `dropped=N` — samples the APU produced that push_samples could not accept; should be 0

### Known-Good Configuration Summary

- Sys clock: 250 MHz (TFT target only; analog stays at 315 MHz)
- I2S sample rate: 48,000 Hz
- `AUDIO_PIO_FRAME_CYCLES = 133` (1 + 2×(1 + 64 + 1) = 133; the constant and comment must agree)
- `pack_symbols` loop: **15** iterations — 16 would shift d[0] off the top of the uint32_t word
- `shift_right = false` (MSB first, left-shift OSR)

## Pico SD ROM / Flash Cache Status

The Pico menu can now load ROMs from SD into the external flash cache and run
one game at a time from the menu. This has been validated informally on both
analog and HDMI targets with many games, including:

- Super Mario Bros
- Tetris
- Pac-Man
- Ninja Gaiden
- Dragon Warrior
- Final Fantasy
- Chip 'n Dale

Important implementation notes:

- Full ROM images do not need to be copied into SRAM.
- ROMs are streamed from SD into the flash cache, then loaded zero-copy from XIP.
- Small PRG ROMs may be copied into SRAM to avoid flash/XIP contention.
- CHR ROM can remain in flash; CHR RAM remains in SRAM.
- The flash cache suspend/resume path must coordinate with the active video backend.

Remaining known issue:

- Some ROM loads still intermittently kill the display signal and require a reboot.
- The failure is more common around flash-cache rewrites than cache hits.
- Analog had a confirmed RP2350 chained-DMA restart issue; the fix is to clear DMA
  channel `EN` bits before aborting chained channels, then reinitialize/restart
  the analog PIO/DMA/core1 path.
- HDMI can still lose video on some loads. A useful clue: when HDMI loses video,
  composite audio can continue, but it sounds scratchy, like frames are being
  dropped or timing is unstable. This suggests the remaining failure may be a
  load-time timing/resource-contention issue rather than the emulator fully
  stopping.
- Future debugging should distinguish:
  - emulator still stepping vs. CPU/PPU stalled
  - HDMI/HSTX frame counter still advancing vs. signal lost
  - audio buffer underrun/overrun during and after flash writes
  - cache hit vs. cache rewrite behavior
  - whether SD streaming, flash erase/program, USB logging, or video resume is
    consuming enough time/bus bandwidth to disturb output timing

## ESP32-S3 Implementation Notes

### Scope

There is also an ESP32-S3 frontend under `src/esp32s3/main/`.

This is a separate platform implementation, not the main portable runtime target.
Treat it as:

- a useful reference for embedded frontend structure
- a reference for MAX98357 wiring and I2S usage
- a source of ideas for backend interfaces

Do not assume ESP32-S3 code can be copied directly into Pico code. The high-level
backend shape carries over; the low-level driver code does not.

### Current ESP32-S3 Layout

Key files:

- `src/esp32s3/main/main.c`
- `src/esp32s3/main/audio.c`
- `src/esp32s3/main/audio.h`
- `src/esp32s3/main/display.c`
- `src/esp32s3/main/board.h`
- `src/esp32s3/main/nes_video.c`
- `src/esp32s3/main/nes_input.c`

### Audio Model

The ESP32-S3 audio path is a good reference for backend structure:

- emulator produces shared-core `int16_t` PCM samples
- platform frontend pushes them into a ring buffer
- a platform-specific output task drains that ring buffer into hardware

In `src/esp32s3/main/audio.c`:

- audio output uses ESP-IDF standard I2S driver APIs
- `audio_init(sample_rate)` configures an I2S TX channel
- a FreeRTOS audio task drains the PCM ring buffer with `i2s_channel_write(...)`
- the slot config is 16-bit mono through the ESP-IDF helper macros

This is a good architecture reference, but not an implementation reference for Pico:

- ESP32-S3 uses ESP-IDF I2S driver APIs
- Pico uses pico-sdk + PIO/DMA
- ESP32-S3 uses FreeRTOS tasks
- Pico audio currently uses IRQ/DMA-driven backend code instead

### ESP32-S3 MAX98357 Wiring

The ESP32-S3 board wiring documented in `src/esp32s3/main/board.h` is:

- `GPIO4`  -> `BCLK`
- `GPIO2`  -> `LRC` / `WS`
- `GPIO3`  -> `DIN`

Board notes there also document:

- `VIN` -> `3.3V` or `5V`
- `GND` -> `GND`
- `OUT+` / `OUT-` -> speaker
- `SD_MODE` left unconnected for the board's mono mix default

When comparing ESP32-S3 vs Pico MAX98357 behavior, use `board.h` as the wiring source of truth for ESP32-S3 and `audio_i2s_max98357.h` for Pico.

### Practical Guidance

When working on embedded platform code:

- keep `src/common/` portable
- treat ESP32-S3 and Pico as separate hardware backends
- copy interface ideas freely
- do not copy driver calls or timing assumptions across platforms without re-deriving them

## Guidelines for Future Changes

- Prefer narrow fixes over broad rewrites.
- Do not add host-only code to `src/common/`.
- Do not add Pico SDK dependencies to the shared core.
- Keep deterministic smoke validation healthy after every meaningful renderer/APU change.
- When debugging rendering:
  - capture PNGs/videos from host tooling
  - compare frame windows numerically when possible
  - instrument exact PPU register writes before assuming generic tile-fetch corruption
- When debugging gameplay stalls:
  - first determine whether CPU, PPU, NMI, frame hashes, and sprite-0 hit are still advancing
  - do not guess broadly without evidence

## Current Repo-Specific Commands

Build host:

```sh
cd /Users/bchelf/microNES
cmake -S . -B build-host -DMICRONES_PLATFORM=host
cmake --build build-host -j
```

Configure Pico:

```sh
cd /Users/bchelf/microNES
source ~/.zshrc
cmake -S . -B build-pico \
  -DMICRONES_PLATFORM=pico \
  -DMICRONES_PICO_VIDEO_MODE=emulator \
  -DMICRONES_PICO_ROM_PATH=/Users/bchelf/microNES/roms/smb1.nes \
  -Dpicotool_DIR=/Users/bchelf/microNES/build/_deps/picotool
```

Build Pico analog target:

```sh
cmake --build build-pico --target micrones_pico_analog -j
```

Build Pico TFT target:

```sh
cmake --build build-pico --target micrones_pico_tft -j
```

Build Pico TFT + MAX98357 target:

```sh
cmake --build build-pico --target micrones_pico_tft_max98357 -j
```

Configure Pico test-pattern builds:

```sh
cd /Users/bchelf/microNES
source ~/.zshrc
cmake -S . -B build-pico-test \
  -DMICRONES_PLATFORM=pico \
  -DMICRONES_PICO_VIDEO_MODE=test_pattern \
  -Dpicotool_DIR=/Users/bchelf/microNES/build/_deps/picotool
```

Build ESP32-S3:

```sh
cd /Users/bchelf/microNES/src/esp32s3
. $IDF_PATH/export.sh
idf.py build
```

Build ESP32-S3 with an explicit ROM:

```sh
cd /Users/bchelf/microNES/src/esp32s3
. $IDF_PATH/export.sh
idf.py -DMICRONES_ROM_PATH=/absolute/path/to/game.nes build
```

Run SDL:

```sh
./build-host/micrones_run roms/smb1.nes
```

Run smoke:

```sh
./build-host/micrones_smoke roms/smb1.nes
```

## Current Bottom Line

`micrones` now has:

- a serious shared SMB1/NROM runtime
- deterministic host validation
- working framebuffer generation
- working sprite-0 hit
- working host PNG/video capture
- working host SDL interaction
- working first-pass audio pipeline

The main remaining technical debt is:

- APU completeness
- deeper PPU accuracy where SMB later requires it
- eventual Pico-side integration/performance work
