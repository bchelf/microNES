# Branch Notes: `ben/make-celeste-work`

## Current baseline

The useful baseline on this branch is commit `108b74f`:

- adds initial mapper 4 / MMC3 support
- allows `roms/celeste.nes` to load, boot, and run
- keeps mapper 0 / mapper 1 support intact
- preserves the existing fast non-MMC3 execution paths

## What `108b74f` includes

- mapper 4 acceptance in the cartridge loader
- MMC3 PRG banking
- MMC3 CHR banking
- MMC3 mirroring writes
- experimental MMC3 IRQ support
- host and ESP32 build integration for `mmc3.c`

## Verified behavior

Using the committed mapper-4 baseline:

- `roms/celeste.nes` boots successfully
- title screen renders correctly
- gameplay is functional enough to play through at least early levels
- host smoke tests run successfully for short and longer runs

Representative check:

```bash
./build-host/micrones_smoke roms/celeste.nes 300000
```

## Known limitations

- MMC3 timing is still approximate, not fully cycle-accurate
- WRAM protect/enable behavior is not fully modeled
- some observed visual quirks may be game-accurate rather than emulator bugs
- deeper PPU timing experiments after `108b74f` were not kept as the baseline

## Recommendation

If future work resumes from this branch, use `108b74f` as the clean starting point and treat any later PPU timing experiments as separate exploratory work.
