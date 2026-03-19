# micrones Agent Notes

This file summarizes the current state of `micrones`, what has already been implemented and validated, what has been learned during debugging, and what remains.

## Project Intent

`micrones` is intentionally narrow:

- It is not a general NES emulator.
- It targets the original Super Mario Bros ROM.
- It only supports mapper 0 / NROM.
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

## What Is Working

### Build / Targets

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

Build Pico:

```sh
cd /Users/bchelf/microNES
source ~/.zshrc
cmake -S . -B build-pico -DMICRONES_PLATFORM=pico -Dpicotool_DIR=/Users/bchelf/microNES/build/_deps/picotool
cmake --build build-pico -j
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

