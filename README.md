# smb2350

This repo now contains two separate tracks:

- a Pico 2 / RP2350 hardware bring-up target for composite video and audio
- Deliverable A: a portable SMB1/NES core library for host-side testing and later Pico reuse

The emulator core is intentionally platform-independent. It does not include Pico SDK headers, macOS-specific APIs, PNG output, or composite video logic.

## Deliverable A

Deliverable A adds a reusable C core under `src/common` with these pieces:

- `Nes` top-level state object and stepping API
- NROM-only iNES cartridge loading
- CPU/bus/reset foundation for real ROM execution
- PPU state and scanline-oriented rendering scaffolding
- controller latch/shift abstraction
- frame buffer and scanline buffer types
- a native host smoke executable for macOS development

### What is intentionally incomplete

This is a serious foundation, but not a finished emulator:

- the CPU implements a substantial real opcode path, not the complete 6502 instruction matrix
- only mapper `0` / NROM is supported
- the PPU is background/scanline oriented, but not cycle-accurate and not sprite-complete
- the APU is a timing/register scaffold only
- there is no PNG output yet
- there is no composite conversion in the emulator core

The design goal is to keep the core deterministic, testable, and easy to reuse from either a host renderer or a future RP2350 scanline output path.

## Repo layout

```text
.
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
    |   `-- smoke_main.c
    `-- pico
        |-- audio_pwm.c
        |-- audio_pwm.h
        |-- main.c
        |-- video_ntsc.c
        |-- video_ntsc.h
        `-- video_ntsc.pio
```

## Architecture

### Portable core

`src/common` contains all emulator/runtime state and logic:

- `nes.c` owns the top-level bus, reset flow, stepping functions, and public API
- `cpu6502.c` executes instructions against the NES bus
- `cart.c` loads iNES files and validates the NROM-only cartridge model
- `nrom.c` maps PRG/CHR memory for mapper 0
- `ppu.c` owns VRAM, OAM, palette RAM, PPU register behavior, frame timing, and a scanline-oriented background renderer
- `input.c` implements the controller latch and serial shift behavior
- `apu.c` is a minimal timing/register placeholder for later audio work

The frame output is a portable indexed-pixel representation:

- full frame buffer: `256x240`
- scanline buffer: `256` pixels

That makes it usable later from:

- a host-side frame hash / PNG dumper
- an RP2350 scanline-to-composite adapter

### Build separation

The top-level CMake now supports one platform per build directory:

- `SMB2350_PLATFORM=host` builds the portable core and native smoke target
- `SMB2350_PLATFORM=pico` builds the existing RP2350 bring-up target

That keeps host testing native while preserving the Pico cross-build.

## Build on macOS

### Host smoke target

This uses the normal host compiler and does not require the Pico SDK toolchain path.

```sh
cd /Users/bchelf/smb2350
cmake -S . -B build-host -DSMB2350_PLATFORM=host
cmake --build build-host -j
./build-host/smb2350_smoke roms/smb1.nes 64
```

What the smoke target does:

1. loads an iNES ROM from the path you give it
2. validates that it is NROM / mapper 0
3. resets the NES core
4. prints the reset vector and initial CPU state
5. executes a small number of CPU instruction steps
6. prints the resulting CPU/PPU summary

### Pico bring-up target

This keeps the current composite/audio bench target intact.

```sh
cd /Users/bchelf/smb2350
source ~/.zshrc
cmake -S . -B build-pico -DSMB2350_PLATFORM=pico
cmake --build build-pico -j
```

The Pico build still produces:

- `build-pico/smb2350.uf2`
- `build-pico/smb2350.elf`
- `build-pico/smb2350.bin`

## Deliverable B likely next

The next useful deliverable is likely:

1. expand CPU opcode coverage until SMB1 reset and main loop execution are dependable
2. improve PPU behavior around scrolling, attribute use, and sprite evaluation
3. add a host-side frame/hash or PNG testing adapter
4. bridge scanline output into the RP2350 composite path without leaking hardware details into the core
