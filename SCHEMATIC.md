# microNES Hardware Schematic

Raspberry Pi Pico 2 (RP2350) running SMB1 with monochrome NTSC composite video output, PWM audio, and a START button.

---

## GPIO Pin Assignments

| GPIO | Function        | Direction |
|------|-----------------|-----------|
| GP0  | Video sync/bias | Output    |
| GP1  | Video luma      | Output    |
| GP2  | Spare button    | Input     |
| GP3  | NES CLOCK       | Output    |
| GP4  | NES LATCH       | Output    |
| GP5  | Audio PWM       | Output    |
| GP6  | NES DATA        | Input     |

---

## Composite Video Output

Monochrome NTSC via a two-resistor DAC. GP0 and GP1 drive the RCA center pin through separate resistors that sum at the jack.

```
GP0 ──[ 1kΩ ]──┐
                ├── RCA center pin (composite out)
GP1 ──[ 470Ω ]─┘

RCA shell ────── GND
```

The display is assumed to provide a 75Ω termination load to ground.

**Voltage levels at the RCA pin (with 75Ω termination):**

| GP0 | GP1 | Level        | NTSC use        |
|-----|-----|--------------|-----------------|
|  0  |  0  | 0 V          | Sync tip        |
|  1  |  0  | ~0.24 V      | Blanking        |
|  0  |  1  | ~0.34 V      | Dark gray       |
|  1  |  1  | ~0.58 V      | White           |

Exact voltages depend on the Pico's 3.3V rail and your display's actual termination.

---

## Audio Output

PWM audio on GP5, filtered to remove the ~984 kHz carrier before the output jack.

```
GP5 ──[ 1kΩ ]──┬── audio jack tip
               ═╧═ 10 nF
                │
               GND
```

- PWM carrier: ~984 kHz (252 MHz sys clock / 256)
- Filter cutoff: ~16 kHz (1kΩ + 10nF RC)
- Output: mono, line level (passive — may need amplification for speakers)

The RC filter is recommended but optional for initial bring-up. Without it you will hear the carrier frequency as a high-pitched whine.

---

## NES Controller (Pico 2 / RP2350)

Original NES controller connected via its 7-pin cable.  The controller
contains a 4021 PISO (parallel-in, serial-out) shift register.

```
NES controller cable
  pin 1  VCC  ─── 3.3 V
  pin 2  CLOCK ── GP3
  pin 3  LATCH ── GP4
  pin 4  DATA  ── GP6   (active-low output from controller)
  pin 5  N/C
  pin 6  N/C
  pin 7  GND  ─── GND
```

- DATA is active-low: the controller pulls it low for each pressed button.
- The Pico's internal pull-up on GP6 keeps the line high (no buttons pressed)
  when no controller is connected.
- No external resistors are needed on CLOCK, LATCH, or DATA.

---

## Spare Button (GP2)

GP2 is no longer used for START (the NES controller provides it).
It remains available as a spare input, e.g. for a reset button.

```
3.3V (internal pull-up via GP2)
  │
GP2 ──── one side of button ──── GND
```

---

## Full Wiring Summary

```
Pico 2 (RP2350)
┌──────────────────────────────────┐
│ GP0 ──[ 1kΩ ]──┐                 │
│ GP1 ──[ 470Ω]──┴── RCA center    │    RCA shell ── GND
│                                  │
│ GP3 ────────────────── NES CLOCK │
│ GP4 ────────────────── NES LATCH │
│ GP6 ────────────────── NES DATA  │
│                                  │
│ GP5 ──[ 1kΩ ]──┬── audio jack tip│
│              [10nF]              │
│                 │                │
│ GND ────────────┴────────────────┘
│  │                               │
│  └── NES GND, RCA shell, jack GND│
└──────────────────────────────────┘
```

---

## Notes

- Power the Pico via USB (5V). No additional power supply is needed.
- The Pico runs overclocked at 252 MHz with VREG at 1.20V — this is within the RP2350 spec at this voltage.
- All signal ground connections (RCA shell, NES controller, audio jack sleeve) must share a common GND with the Pico.
- The composite output is monochrome only. Color is not supported.
- The NES controller is also supported on the ESP32-S3 target; see board.h for those pin assignments (GPIO 11/12/13).
