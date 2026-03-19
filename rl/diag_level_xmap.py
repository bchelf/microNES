"""
Diagnostic: run a scripted agent through a level and print world_x + screen_y
at every step so you can identify x coordinates for key structural positions.

Usage (from repo root):
    python rl/diag_level_xmap.py --rom roms/smb1.nes --level 1-3

Output: one line per env step:
    step=  42  x=  384  sy= 208  ground=1  action=run_right
         ^        ^       ^         ^
         env step world_x screen_y  on solid ground?

At the end, prints a summary of notable x positions
(where y changed significantly = likely a platform edge or gap).

Also saves a video to diag_xmap_<level>.mp4 if the render lib is available.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))

import numpy as np

from smb_env import SMBEnv, ACTION_BUTTONS, NES_BUTTON_RIGHT, NES_BUTTON_B, NES_BUTTON_A


# Button combos used by the scripted agent (cycles through these)
# run_right most of the time, jump periodically to clear obstacles
ACTION_RUN   = 2   # RIGHT + B
ACTION_JUMP  = 4   # RIGHT + B + A
ACTION_NOOP  = 0


def _find_render_lib() -> str | None:
    repo_root = Path(__file__).parent.parent
    for d in [repo_root / "build-host", repo_root / "build"]:
        for suf in [".dylib", ".so"]:
            p = d / f"libmicrones_rl_render{suf}"
            if p.exists():
                return str(p)
    return None


def run(rom_path: str, level: str, lib_path: str | None, max_steps: int, video_out: str | None):
    render_lib = lib_path or _find_render_lib()
    render_mode = "rgb_array" if render_lib else None

    env = SMBEnv(
        rom_path    = rom_path,
        lib_path    = render_lib,
        render_mode = render_mode,
        frame_skip  = 3,
        levels      = [level],
    )

    # Force FULL_LEVEL_START (x=0) so we see the whole level.
    obs, info = env.reset(options={"level": level})

    frames: list[np.ndarray] = []
    records: list[dict]      = []   # (step, x, sy, on_ground)

    prev_sy   = -1
    jump_hold = 0   # frames remaining to hold jump button

    print(f"\nRunning scripted agent on {level} (max {max_steps} steps)...")
    print(f"{'step':>6}  {'x':>6}  {'sy':>4}  {'ground':>6}  action")
    print("-" * 46)

    for step in range(max_steps):
        # Read current state from obs
        # player_state[2] = mario_sy/255, player_state[5] = on_ground
        sy       = int(round(float(obs["player_state"][2]) * 255))
        on_ground = float(obs["player_state"][5]) > 0.5
        wx        = info.get("world_x", 0) if step > 0 else env._read_world_x()

        # Scripted policy: mostly run right, jump every 24 steps
        if jump_hold > 0:
            action = ACTION_JUMP
            jump_hold -= 1
            action_name = "jump_right"
        elif step % 24 == 0:
            action = ACTION_JUMP
            jump_hold = 10
            action_name = "jump_right"
        else:
            action = ACTION_RUN
            action_name = "run_right"

        obs, reward, terminated, truncated, info = env.step(action)
        wx = info.get("world_x", wx)
        sy = int(round(float(obs["player_state"][2]) * 255))
        on_ground = float(obs["player_state"][5]) > 0.5

        records.append({"step": step, "x": wx, "sy": sy, "ground": on_ground})

        # Print every step (redirect to file if too noisy)
        print(f"{step:>6}  {wx:>6}  {sy:>4}  {int(on_ground):>6}  {action_name}")

        frame = env.render()
        if frame is not None:
            frames.append(frame)

        if terminated or truncated:
            reason = "died" if obs["game_flags"][0] > 0.5 else \
                     "complete" if obs["game_flags"][1] > 0.5 else "truncated"
            print(f"\n--- Episode ended at step={step} x={wx}  reason={reason} ---")
            break

    env.close()

    # --- Summary: positions where screen_y changed by >= 20px ---
    print(f"\n{'='*60}")
    print(f"Significant y-changes (platform edges / gaps / drops):")
    print(f"{'='*60}")
    print(f"{'step':>6}  {'x':>6}  {'sy':>4}  {'dy':>5}  note")
    prev_sy = records[0]["sy"] if records else 0
    for r in records:
        dy = r["sy"] - prev_sy
        if abs(dy) >= 20:
            note = "DROP" if dy > 0 else "RISE"
            print(f"{r['step']:>6}  {r['x']:>6}  {r['sy']:>4}  {dy:>+5}  {note}")
        prev_sy = r["sy"]

    # --- Summary: every 50px of forward progress ---
    print(f"\n{'='*60}")
    print(f"x milestones (every 50px):")
    print(f"{'='*60}")
    last_milestone = -50
    for r in records:
        if r["x"] >= last_milestone + 50:
            last_milestone = (r["x"] // 50) * 50
            print(f"  x={last_milestone:>5}  step={r['step']:>5}  sy={r['sy']:>3}  ground={int(r['ground'])}")

    # --- Save video ---
    if frames and video_out:
        try:
            import imageio
            fps = max(1, 60 // 3)
            with imageio.get_writer(video_out, fps=fps, codec="libx264", quality=8) as writer:
                for f in frames:
                    writer.append_data(f)
            print(f"\nVideo saved → {video_out}")
        except Exception as e:
            print(f"\nVideo save failed: {e}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Level x-coordinate map diagnostic")
    parser.add_argument("--rom",       required=True, help="Path to SMB1 .nes ROM")
    parser.add_argument("--level",     default="1-3", help="Level to run (default: 1-3)")
    parser.add_argument("--lib",       default=None,  help="Path to render lib (auto-detected)")
    parser.add_argument("--max-steps", type=int, default=3000, help="Max env steps (default: 3000)")
    parser.add_argument("--video",     default=None,  help="Save video to this path (e.g. diag_13.mp4)")
    args = parser.parse_args()

    video_path = args.video
    if video_path is None and _find_render_lib():
        level_tag  = args.level.replace("-", "_")
        video_path = f"diag_xmap_{level_tag}.mp4"

    run(args.rom, args.level, args.lib, args.max_steps, video_path)
