"""
Diagnose castle-level end detection.

Runs a policy on 1-4 (or any castle level) and dumps all RAM addresses that
change from their steady-state gameplay values, looking specifically for signals
that fire when Mario touches the axe.

Background: RAM[0x001D]==0x03 is the "flagpole slide" Player_State, not a
generic "level complete" signal. Castle levels never enter this state, so
SMBEnv never sets level_complete=True for them. This script identifies which
RAM byte(s) the NES *does* set during the castle-end sequence.

Usage:
    python rl/diag_castle_end.py --rom roms/smb1.nes --checkpoint checkpoints/model_latest.zip
    python rl/diag_castle_end.py --rom roms/smb1.nes --checkpoint ... --level 2-4
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

import numpy as np
from stable_baselines3 import PPO

from smb_env import SMBEnv
from wrappers import AirborneActionMaskWrapper, StickyActionWrapper


# Addresses to annotate in output
KNOWN = {
    0x001D: "Player_State      (0x03=flagpole, want castle equivalent)",
    0x0770: "OperMode          (1=gameplay)",
    0x0772: "OperMode_Task",
    0x0773: "in_transition",
    0x075F: "WorldNumber",
    0x075C: "LevelNumber",
    0x0750: "PackedAreaVal",
    0x0760: "AreaOffset",
    0x074E: "area_type",
    0x074F: "area_num",
    0x000E: "Player_AnimState  (0x0B=death)",
    0x0756: "PowerState",
    0x075A: "Lives",
}


def _find_render_lib():
    from pathlib import Path
    repo = Path(__file__).parent.parent
    for d in [repo / "build-host", repo / "build"]:
        for suf in [".dylib", ".so"]:
            p = d / f"libmicrones_rl_render{suf}"
            if p.exists():
                return str(p)
    raise FileNotFoundError("libmicrones_rl_render not found")


def main():
    parser = argparse.ArgumentParser(description="Diagnose castle level-end RAM signals")
    parser.add_argument("--rom",        required=True)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--level",      default="1-4", help="Castle level to test (default: 1-4)")
    parser.add_argument("--max-steps",  type=int, default=8000)
    parser.add_argument("--lib",        default=None)
    args = parser.parse_args()

    render_lib = args.lib or _find_render_lib()

    env = SMBEnv(rom_path=args.rom, lib_path=render_lib, render_mode=None)
    env = AirborneActionMaskWrapper(env)
    env = StickyActionWrapper(env, sticky_prob=0.0)

    model = PPO.load(args.checkpoint, env=env)
    model.policy.set_training_mode(False)

    obs, _ = env.reset(options={"level": args.level})

    base = env.unwrapped
    ram  = base._ram

    # Capture "baseline" RAM after reset — we'll watch for deviations
    baseline = np.array(ram, dtype=np.uint8).copy()

    # Track per-address history of values that have changed from baseline
    # {addr: list of (step, old_val, new_val)}
    history: dict[int, list] = {}
    prev_snap = baseline.copy()

    print(f"\nRunning policy on {args.level} for up to {args.max_steps} steps...")
    print(f"Monitoring RAM for changes. Key addresses annotated.\n")

    terminated = truncated = False
    last_info = {}
    step = 0

    for step in range(args.max_steps):
        action, _ = model.predict(obs, deterministic=True)
        obs, reward, terminated, truncated, info = env.step(int(action))
        last_info = info

        # Snapshot current RAM
        snap = np.array(ram, dtype=np.uint8).copy()
        changed = np.where(snap != prev_snap)[0]

        for addr in changed:
            old_v = int(prev_snap[addr])
            new_v = int(snap[addr])
            if addr not in history:
                history[addr] = []
            history[addr].append((step, old_v, new_v))

        prev_snap = snap

        if terminated or truncated:
            break

    env.close()

    # ---------- report ----------
    term_type = (
        "LEVEL_COMPLETE (terminated=True, game_flags[1]=True)" if terminated and obs["game_flags"][1] > 0.5 else
        "DEATH (terminated=True, game_flags[0]=True)"          if terminated and obs["game_flags"][0] > 0.5 else
        "TRUNCATED (stagnation/timeout)"                        if truncated else
        "MAX_STEPS reached"
    )

    print(f"Episode ended at step {step}: {term_type}")
    print(f"world_x={last_info.get('world_x', '?')}  on_ground={last_info.get('on_ground', '?')}")
    print(f"RAM[0x001D] at end = 0x{int(prev_snap[0x001D]):02X}  "
          f"RAM[0x0770] at end = 0x{int(prev_snap[0x0770]):02X}  "
          f"RAM[0x0772] at end = 0x{int(prev_snap[0x0772]):02X}")

    # Print final values of all addresses that changed at any point
    all_changed = sorted(history.keys())
    print(f"\n{len(all_changed)} addresses changed during the run.\n")

    print("=== KEY ADDRESSES (known) ===")
    for addr in sorted(KNOWN.keys()):
        baseline_v = int(baseline[addr])
        final_v    = int(prev_snap[addr])
        n_changes  = len(history.get(addr, []))
        marker     = " <-- CHANGED" if final_v != baseline_v else ""
        print(f"  ${addr:04X}  baseline={baseline_v:3d}  final={final_v:3d}  "
              f"changes={n_changes:4d}  {KNOWN[addr]}{marker}")

    # Print every address that was at baseline at reset but changed just before episode end
    # (within the last 200 steps) — these are the most likely level-end signals
    print(f"\n=== ADDRESSES THAT CHANGED IN LAST 200 STEPS (potential end-signal candidates) ===")
    candidates = []
    for addr, events in history.items():
        last_change_step = events[-1][0]
        if last_change_step >= step - 200:
            last_val = events[-1][2]
            first_val = int(baseline[addr])
            candidates.append((addr, first_val, last_val, last_change_step, events))

    candidates.sort(key=lambda x: x[3])  # sort by step of last change

    if not candidates:
        print("  (none — episode ended too quickly or no late changes)")
    else:
        print(f"  {'addr':>6}  {'baseline':>8}  {'final':>5}  {'last_chg_step':>13}  note")
        for addr, bv, lv, lcs, events in candidates:
            note = KNOWN.get(addr, "")
            print(f"  ${addr:04X}  {bv:8d}  {lv:5d}  {lcs:13d}  {note}")
            # Show last few transitions
            for ev_step, ev_old, ev_new in events[-3:]:
                print(f"           step {ev_step}: {ev_old:#04x} → {ev_new:#04x}")

    print(f"\nConclusion:")
    print(f"  RAM[0x001D] at end = 0x{int(prev_snap[0x001D]):02X} "
          f"({'0x03 detected — flagpole fired (unexpected for castle)' if prev_snap[0x001D] == 3 else 'never 0x03 — confirms flagpole detection does not fire'}) ")
    print(f"  Look for an address above that transitions once near step {step} "
          f"and stays at a non-zero/non-baseline value.")
    print(f"  That address (and value) is the signal to add to smb_env.py's level_complete detection.")


if __name__ == "__main__":
    main()
