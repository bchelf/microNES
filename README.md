# smb2350

Minimal Raspberry Pi Pico 2 / RP2350 bring-up project for the first two console hardware checks:

- monochrome NTSC composite video on `GP0` and `GP1`
- mono PWM audio tone on `GP2`

This project intentionally avoids emulator work, color encoding, and extra hardware so the first UF2 can verify that video lock and audio output work on the bench.

## Project layout

```text
.
|-- CMakeLists.txt
|-- pico_sdk_import.cmake
|-- README.md
`-- src
    |-- audio_pwm.c
    |-- audio_pwm.h
    |-- main.c
    |-- video_ntsc.c
    |-- video_ntsc.h
    `-- video_ntsc.pio
```

## Build on macOS

Assumptions:

- Pico SDK is already installed
- `PICO_SDK_PATH` is exported in your shell
- ARM embedded build tools are already installed

Configure and build:

```sh
mkdir -p build
cd build
cmake ..
cmake --build . -j
```

The build produces:

- `build/smb2350.uf2`
- `build/smb2350.elf`
- `build/smb2350.bin`

## Flashing the UF2

1. Hold `BOOTSEL` on the Pico 2 while plugging in USB.
2. The board should mount as `RPI-RP2`.
3. Copy `build/smb2350.uf2` onto that drive.
4. The board will reboot and start video/audio output immediately.

## Breadboard wiring

### Composite video

Simple 2-pin resistor DAC into one RCA jack:

- `GP0` -> `1k` resistor -> RCA center
- `GP1` -> `470 ohm` resistor -> RCA center
- Pico `GND` -> RCA shell

Level assumptions:

- `GP0` is the sync/blank bias pin
- `GP1` adds luma for white pixels
- the display is assumed to provide `75 ohm` termination internally

Nominal output levels with that load are roughly:

- sync: both pins low
- black / blank: `GP0=1`, `GP1=0`
- white: `GP0=1`, `GP1=1`

### Audio

- `GP2` -> `1k` resistor -> audio jack tip
- Pico `GND` -> audio jack sleeve

An optional small RC low-pass is fine, but the tone is designed to be audible even with only the series resistor.

## What you should see and hear

On power-up the program generates a non-interlaced NTSC-like monochrome test frame:

- black background
- white border
- vertical bar region for width/linearity checks
- horizontal bar region for height checks
- white center crosshair

You should hear a steady mid-frequency test tone on `GP2`.

This is intended as a first hardware confidence check:

- CRT or composite monitor should lock horizontally and vertically
- geometry should be visibly inspectable
- audio path should confirm the PWM output wiring

## Implementation notes

- Video timing uses a PIO state machine clocked at `512 samples / line`, with DMA continuously looping over a precomputed frame buffer.
- The frame is `262` lines long at approximately `59.94 Hz`, matching a simple `240p`-style NTSC cadence rather than full interlaced broadcast timing.
- Vertical sync is intentionally simplified to a short broad-sync region because the goal here is reliable bring-up, not broadcast compliance.
- Audio uses high-rate PWM on `GP2` and a timer-driven sine table for a simple test tone.

## Next milestones

1. Replace the static test frame with a scanline renderer and line callbacks.
2. Add a tile/sprite-oriented monochrome SMB test renderer.
3. Replace the single tone with a small software mixer and frame-synchronous audio.
4. Decide whether to keep the PIO + DMA composite path or move to a more advanced renderer once gameplay timing is integrated.
