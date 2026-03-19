# microNES Hardware Schematic

Raspberry Pi Pico 2 (RP2350) running SMB1 with monochrome NTSC composite video output, PWM audio, and a START button.

---

## GPIO Pin Assignments

| GPIO | Function       | Direction |
|------|----------------|-----------|
| GP0  | Video sync/bias | Output   |
| GP1  | Video luma      | Output   |
| GP2  | START button    | Input     |
| GP5  | Audio PWM       | Output   |

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

## START Button

Momentary pushbutton between GP2 and GND. The Pico's internal pull-up holds GP2 high when the button is not pressed.

```
3.3V (internal pull-up via GP2)
  │
GP2 ──────────── one side of button
                 other side of button ── GND
```

- Not pressed: GP2 = HIGH → START not asserted
- Pressed: GP2 = LOW → START asserted

No external resistor is needed. Do not connect a pull-down — the internal pull-up handles it.

---

## Full Wiring Summary

```
Pico 2 (RP2350)
┌─────────────────────┐
│ GP0 ──[ 1kΩ ]──┐    │
│ GP1 ──[ 470Ω]──┴── RCA center
│                     │    RCA shell ── GND
│ GP2 ──────────── [BTN] ── GND
│                     │
│ GP5 ──[ 1kΩ ]──┬── audio jack tip
│              [10nF]  │
│                 │    │
│ GND ───────────┴────┘
└─────────────────────┘
```

---

## Notes

- Power the Pico via USB (5V). No additional power supply is needed.
- The Pico runs overclocked at 252 MHz with VREG at 1.20V — this is within the RP2350 spec at this voltage.
- All signal ground connections (RCA shell, button, audio jack sleeve) must share a common GND with the Pico.
- The composite output is monochrome only. Color is not supported.
