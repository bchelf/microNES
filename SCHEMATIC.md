# microNES Hardware Schematic

Raspberry Pi Pico 2 (RP2350) running SMB1 with color NTSC composite video output, PWM audio, and a hardware NES controller.

---

## GPIO Pin Assignments

| GPIO | Function              | Direction |
|------|-----------------------|-----------|
| GP0  | Video DAC bit 0 (LSB) | Output    |
| GP1  | Video DAC bit 1       | Output    |
| GP2  | Video DAC bit 2       | Output    |
| GP3  | Video DAC bit 3 (MSB) | Output    |
| GP4  | Sync clamp MOSFET gate| Output    |
| GP5  | NES CLOCK             | Output    |
| GP6  | NES LATCH             | Output    |
| GP7  | NES DATA              | Input     |
| GP9  | Audio PWM             | Output    |

---

## Composite Video Output

Color NTSC via a 4-bit binary-weighted resistor DAC on GP0–GP3. A MOSFET on GP4 clamps the output to GND during sync pulses for a clean sync tip.

```
GP0 ──[ 1000Ω ]──┐
GP1 ──[  485Ω ]──┤  (470Ω + 15Ω)
GP2 ──[  242Ω ]──┤  (220Ω + 22Ω)
GP3 ──[  120Ω ]──┴── Node A ──[ 75.5Ω series ]── RCA center pin

GP4 ──── MOSFET gate  (N-channel; drain → Node A, source → GND)

RCA shell ──── GND
```

The display provides a 75Ω termination load to ground. The 75.5Ω series resistor plus 75Ω load form a matched output impedance.

**DAC codes (analytically derived at 315 MHz / Node A with 75Ω load):**

| Code | Voltage at RCA | NTSC use              |
|------|----------------|-----------------------|
|  0   | 0.000 V        | Sync tip (GP4 clamp active) |
|  4   | 0.306 V        | Blanking / back porch |
| 13   | 0.998 V        | White luma            |
| 15   | 1.151 V        | Peak chroma headroom  |

Luma range: codes 4–13 (blank to white), scale factor 9 per step.
Chroma: ±2–3 DAC codes added/subtracted on active pixels, 13 hues × 4 subcarrier phases.

---

## Sync Clamp (GP4)

An N-channel MOSFET (e.g. 2N7002 or BSS138) pulls Node A to GND during sync intervals, ensuring the sync tip reaches true 0 V regardless of DAC resistor tolerances.

```
Node A ──── DRAIN
             │
            MOSFET (N-ch)
             │
GP4  ──── GATE
             │
            SOURCE ──── GND
```

GP4 is driven HIGH by the PIO DMA IRQ handler at the start of each sync interval and LOW ~1 µs later by Core 1.

---

## Audio Output

PWM audio on GP9, filtered to remove the 44 kHz carrier before the output jack.

```
GP9 ──[ 1kΩ ]──┬──[ 1kΩ ]──┬── 100µF (DC block) ──┬── audio jack tip
              [10nF]       [10nF]              [10kΩ to GND]
                │             │
               GND           GND
```

- PWM carrier: 44,103 Hz (315 MHz / 7143 — nearest integer to 44,100 Hz, 75 ppm error)
- Two-pole RC low-pass filter: R=1kΩ, C=10nF each pole, f_c ≈ 15.9 kHz
- 100µF DC-blocking cap + 10kΩ bleeder to GND on output
- BAT85 ESD diode pair recommended on the output jack
- Output: mono, line level (passive — may need amplification for speakers)

---

## NES Controller (Pico 2 / RP2350)

Original NES controller connected via its 7-pin cable. The controller contains a 4021 PISO (parallel-in, serial-out) shift register.

```
NES controller cable
  pin 1  VCC   ─── 3.3 V
  pin 2  CLOCK ─── GP5
  pin 3  LATCH ─── GP6
  pin 4  DATA  ─── GP7   (active-low output from controller)
  pin 5  N/C
  pin 6  N/C
  pin 7  GND   ─── GND
```

- DATA is active-low: the controller pulls it low for each pressed button.
- The Pico's internal pull-up on GP7 keeps the line high (no buttons pressed) when no controller is connected.
- No external resistors are needed on CLOCK, LATCH, or DATA.

---

## Full Wiring Summary

```
Pico 2 (RP2350)
┌──────────────────────────────────────────────────────┐
│ GP0 ──[ 1000Ω ]──┐                                   │
│ GP1 ──[  485Ω ]──┤                                   │
│ GP2 ──[  242Ω ]──┤                                   │
│ GP3 ──[  120Ω ]──┴── Node A ──[ 75.5Ω ]── RCA center│   RCA shell ── GND
│ GP4 ─────────────────── MOSFET gate (drain→Node A)   │
│                                                      │
│ GP5 ────────────────────────────────── NES CLOCK     │
│ GP6 ────────────────────────────────── NES LATCH     │
│ GP7 ────────────────────────────────── NES DATA      │
│                                                      │
│ GP9 ──[1kΩ]──[1kΩ]──[100µF]──[10kΩ↓]── audio jack  │
│         │      │                                     │
│        10nF   10nF                                   │
│         │      │                                     │
│ GND ────┴──────┴─────────────────────────────────────┘
│  │
│  └── NES GND, RCA shell, MOSFET source, jack GND
└─────────────────────────────────────────────────────────
```

---

## Notes

- Power the Pico via USB (5V). No additional power supply is needed.
- The Pico runs at exactly 315 MHz (PLL: VCO=1260 MHz, post_div1=2, post_div2=2) with VREG at 1.20V — within RP2350 spec.
- System clock is load-bearing: 315 MHz / 22 = 14.318182 MHz NTSC sample rate (colorburst × 4); 315 MHz / 7143 = 44,103 Hz audio PWM rate.
- All signal ground connections (RCA shell, NES controller, audio jack sleeve, MOSFET source) must share a common GND with the Pico.
- The composite output supports color. Chroma is generated by adding/subtracting DAC codes in sync with the 3.579545 MHz NTSC subcarrier (4 phases × 14.318 MHz ÷ 4).
- The NES controller is also supported on the ESP32-S3 target; see board.h for those pin assignments (GPIO 11/12/13).
