# microNES

`microNES` is a narrow NES runtime project aimed at running the original Super Mario Bros on RP2350 / Raspberry Pi Pico 2.

It is intentionally not a general NES emulator.

Current project goals:

- keep the emulator core portable and deterministic
- use the real SMB1 ROM, not a hand-ported rewrite
- validate behavior on the host first
- reuse the same core later on RP2350

## Current Status

The repo now has three active pieces:

- a shared portable NES/SMB1 core in `src/common`
- host-side tooling in `src/host`
- an RP2350 / Pico 2 firmware target in `src/pico`

What is working now:

- SMB1 ROM loading for mapper 0 / NROM
- deterministic CPU execution for SMB1
- background rendering and minimal sprite composition
- working sprite-0 hit, including the later gameplay case
- a visible `256x240` framebuffer in the shared core
- host smoke validation with hashes and diagnostics
- host PNG export
- host ffmpeg video capture
- host SDL interactive window
- host keyboard input for SMB1
- host approximate color presentation
- shared-core PCM sample generation
- host SDL audio playback

The host-side project is now good enough to:

- boot and run SMB1
- inspect frames and video captures
- play the game locally in a window
- hear recognizable SMB audio
- debug rendering and audio behavior with instrumentation

## Repo Layout

```text
.
|-- AGENT.md
|-- CMakeLists.txt
|-- pico_sdk_import.cmake
|-- README.md
`-- src
    |-- common
    |   |-- apu.c
    |   |-- apu.h
    |   |-- cart.c
    |   |-- cart.h
    |   |-- cpu6502.c
    |   |-- cpu6502.h
    |   |-- cpu6502_opcode.c
    |   |-- cpu6502_opcode.h
    |   |-- frame_pacer.c
    |   |-- frame_pacer.h
    |   |-- framebuffer.h
    |   |-- input.c
    |   |-- input.h
    |   |-- nes.c
    |   |-- nes.h
    |   |-- nrom.c
    |   |-- nrom.h
    |   |-- ppu.c
    |   |-- ppu.h
    |   `-- scanline.h
    |-- host
    |   |-- audio_sdl.c
    |   |-- audio_sdl.h
    |   |-- png_write.c
    |   |-- png_write.h
    |   |-- run_main.c
    |   |-- smoke_main.c
    |   |-- video_capture.c
    |   |-- video_capture.h
    |   |-- wav_write.c
    |   |-- wav_write.h
    |   |-- window_sdl.c
    |   `-- window_sdl.h
    `-- pico
        |-- audio_pwm.c
        |-- audio_pwm.h
        |-- main.c
        |-- pico_time.c
        |-- pico_time.h
        |-- video_ntsc.c
        |-- video_ntsc.h
        `-- video_ntsc.pio
```

## Architecture

### Shared Core

`src/common` contains the portable emulator/runtime logic:

- `nes.c` owns the top-level NES state, bus, reset, stepping, and public API
- `cpu6502.c` executes SMB1 against the NES bus
- `cart.c` and `nrom.c` handle iNES loading and mapper 0
- `ppu.c` owns PPU registers, VRAM/OAM/palette state, frame timing, background rendering, sprite composition, and sprite-0 hit behavior
- `apu.c` owns audio timing, channel state, PCM sample generation, and audio debug instrumentation
- `frame_pacer.c` provides portable pacing/timing policy
- `input.c` handles controller latch/shift behavior

The shared core does not depend on:

- SDL
- Pico SDK
- host-only file/image/audio/window APIs

### Host Side

`src/host` contains tools and adapters only for local development:

- `smb2350_smoke`
  - deterministic validation
  - state and frame hashes
  - debug summaries
  - PNG output
  - ffmpeg video capture
- `smb2350_run`
  - live SDL window
  - keyboard input
  - host-side color mapping
  - SDL audio playback
  - WAV dumping
  - APU diagnostics

### Pico Side

`src/pico` is still a separate RP2350 target. It builds successfully and remains useful as the hardware-specific path for later integration work.

## Build

### Host

```sh
cd /Users/bchelf/smb2350
cmake -S . -B build-host -DSMB2350_PLATFORM=host
cmake --build build-host -j
```

This builds:

- `build-host/smb2350_smoke`
- `build-host/smb2350_run`

### Pico

```sh
cd /Users/bchelf/smb2350
source ~/.zshrc
cmake -S . -B build-pico -DSMB2350_PLATFORM=pico -Dpicotool_DIR=/Users/bchelf/smb2350/build/_deps/picotool
cmake --build build-pico -j
```

This builds:

- `build-pico/smb2350.uf2`
- `build-pico/smb2350.elf`
- `build-pico/smb2350.bin`

## Useful Host Commands

### Smoke Validation

Run a deterministic bounded validation:

```sh
./build-host/smb2350_smoke roms/smb1.nes
```

Run longer and dump a frame:

```sh
./build-host/smb2350_smoke roms/smb1.nes 6200000 /tmp/smb2350_10s.png
```

Capture video through ffmpeg:

```sh
./build-host/smb2350_smoke roms/smb1.nes --steps 17670500 --video-out build-host/smb_30s.mp4
```

### Interactive Runner

Run the SDL window:

```sh
./build-host/smb2350_run roms/smb1.nes
```

Helpful options:

```sh
./build-host/smb2350_run roms/smb1.nes --vsync
./build-host/smb2350_run roms/smb1.nes --no-vsync --unthrottled
./build-host/smb2350_run roms/smb1.nes --scale 5
./build-host/smb2350_run roms/smb1.nes --max-frames 600
```

Keyboard mapping:

- `Up` or `W` = Up
- `Down` or `S` = Down
- `Left` or `A` = Left
- `Right` or `D` = Right
- `L` = A
- `K` = B
- `Return` = Start
- `Tab` or `Right Shift` = Select

Host-side input conflict handling:

- `left + right` becomes no-op
- `up + down` becomes no-op

The host runner now also polls input much more frequently than once per frame, which reduces the control lag that earlier versions had.

## Audio Diagnostics Harness

The repo now includes a small audio debug harness to make APU changes measurable instead of subjective.

Available capabilities:

- per-channel solo / mute in the final mix
- forced test tones
- pre-SDL WAV dumping
- per-channel and final mix amplitude statistics
- APU register write summaries
- compact channel state summaries

Examples:

Dump a short gameplay window to WAV:

```sh
./build-host/smb2350_run roms/smb1.nes --dump-wav /tmp/smb_gameplay.wav --dump-wav-seconds 2
```

Solo a channel:

```sh
./build-host/smb2350_run roms/smb1.nes --audio-solo pulse1 --apu-stats
./build-host/smb2350_run roms/smb1.nes --audio-solo triangle --apu-stats
```

Mute a channel:

```sh
./build-host/smb2350_run roms/smb1.nes --audio-mute triangle --apu-stats
```

Forced validation tones:

```sh
./build-host/smb2350_run roms/smb1.nes --audio-solo pulse1 --apu-test-tone pulse1 --dump-wav /tmp/pulse1.wav --dump-wav-seconds 1 --apu-stats
./build-host/smb2350_run roms/smb1.nes --audio-solo triangle --apu-test-tone triangle --dump-wav /tmp/triangle.wav --dump-wav-seconds 1 --apu-stats
```

Write summary:

```sh
./build-host/smb2350_run roms/smb1.nes --apu-write-summary
```

Current main conclusion from the harness:

- the shared PCM path and host playback path are real and testable
- earlier “no audible difference” checks were partly misleading because some short no-input SMB windows did not meaningfully drive the audible channels

## Important Rendering / Debugging Findings

Some of the major issues already diagnosed and fixed:

- early SMB1 progress was blocked by missing sprite-0 hit behavior
- a later gameplay stall was caused by incorrect background/scroll/address derivation, not by sprite corruption
- a later visible HUD/title-card artifact was traced to visible-scanline scroll writes being ignored by the deferred scanline renderer

Those fixes were kept narrow and SMB-focused rather than turning the project into a broad emulator rewrite.

## Known Limitations

This is still an intentionally incomplete SMB-focused runtime.

### CPU / Cartridge

- only mapper 0 / NROM is supported
- not all 6502 behavior is implemented for arbitrary ROMs
- SMB1 is the target, not broad emulator compatibility

### PPU

- not cycle-accurate
- no exact secondary OAM behavior
- no sprite overflow behavior
- no full final NES palette implementation in the shared core
- host color display is still an approximation layered over the shared framebuffer

### APU

- audible SMB audio exists, but fidelity is still incomplete
- bass / triangle behavior is still not where it should be
- some sound effects still feel clipped or abbreviated
- the next APU work should use the diagnostics harness rather than more blind tuning

### Host Timing

- the SDL runner is much more playable now, but pacing and vsync tradeoffs still matter
- `--vsync` usually looks better
- `--no-vsync --unthrottled` is still useful for perf/debug work

## Next Steps

Highest-value next work:

1. Use the audio diagnostics harness during real gameplay windows to identify which channel behavior is still missing or too approximate.
2. Improve the APU with evidence, especially:
   - triangle behavior
   - envelope / length timing
   - noise / fuller SMB effects
3. Keep tightening SMB-relevant PPU correctness only where new evidence shows it is needed.
4. Reuse the shared PCM and pacing abstractions for the future Pico runtime path.
5. Move toward RP2350 integration only after host-side validation remains strong.

The project is now beyond early bring-up. The main remaining work is no longer “make it boot”; it is “make SMB behavior and output more faithful while preserving the narrow, portable architecture.”
