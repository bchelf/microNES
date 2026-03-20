"""
Record deterministic episode(s) for one level using a saved PPO checkpoint.

Usage:
    python rl/record_level.py --rom roms/smb1.nes --model checkpoints/model_0000500000.zip
    python rl/record_level.py --rom roms/smb1.nes --model my_checkpoint.zip --level 1-3
    python rl/record_level.py --rom roms/smb1.nes --model my_checkpoint.zip --level 1-3 \\
        --n-episodes 3 --out 1-3_run.mp4

Options:
    --rom PATH          Path to SMB1 iNES ROM (required)
    --model PATH        Path to checkpoint .zip file (required)
    --level LEVEL       Level to play (default: 1-1)
    --n-episodes N      Number of episodes to record; best run saved to video (default: 1)
    --out PATH          Output MP4 path (default: <level>_<model_stem>.mp4)
    --max-steps N       Max env steps per episode (default: 5000)
    --fps N             Output video FPS (default: 20)
    --lib PATH          Path to libmicrones_rl_render (auto-detected if omitted)
"""

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))

import imageio
import numpy as np
from stable_baselines3 import PPO

from smb_env import SMBEnv


def find_render_lib() -> str:
    repo_root = Path(__file__).parent.parent
    for d in [repo_root / "build-host", repo_root / "build"]:
        for suf in [".dylib", ".so"]:
            p = d / f"libmicrones_rl_render{suf}"
            if p.exists():
                return str(p)
    raise FileNotFoundError(
        "libmicrones_rl_render not found. Build with:\n"
        "  cmake --build build-host -j --target micrones_rl_render"
    )


def run_episode(model: PPO, env: SMBEnv, level: str, max_steps: int) -> dict:
    obs, _ = env.reset(options={"level": level})
    frames: list[np.ndarray] = []
    total_reward = 0.0
    max_world_x  = 0
    completed    = False

    for step in range(max_steps):
        action, _ = model.predict(obs, deterministic=True)
        obs, reward, terminated, truncated, info = env.step(int(action))
        total_reward += float(reward)

        wx = info.get("world_x", 0)
        if wx > max_world_x:
            max_world_x = wx

        frames.extend(env.pop_step_frames())

        if terminated and obs["game_flags"][1] > 0.5:
            completed = True

        if terminated or truncated:
            break

    return {
        "frames":       frames,
        "total_reward": total_reward,
        "max_world_x":  max_world_x,
        "completed":    completed,
        "steps":        step + 1,
    }


def main():
    parser = argparse.ArgumentParser(description="Record a deterministic eval episode")
    parser.add_argument("--rom",        required=True, help="Path to SMB1 .nes ROM")
    parser.add_argument("--model",      required=True, help="Path to checkpoint .zip")
    parser.add_argument("--level",      default="1-1", help="Level to play (default: 1-1)")
    parser.add_argument("--n-episodes", type=int, default=1,
                        help="Episodes to run; best by max_x saved to video (default: 1)")
    parser.add_argument("--out",        default=None,  help="Output MP4 path")
    parser.add_argument("--max-steps",  type=int, default=5000)
    parser.add_argument("--fps",        type=int, default=20)
    parser.add_argument("--lib",        default=None,  help="Path to libmicrones_rl_render")
    args = parser.parse_args()

    lib_path = args.lib or find_render_lib()

    if args.out:
        out_path = args.out
    else:
        level_tag  = args.level.replace("-", "_")
        model_stem = Path(args.model).stem
        out_path   = f"{level_tag}_{model_stem}.mp4"

    print(f"ROM:        {args.rom}")
    print(f"Model:      {args.model}")
    print(f"Level:      {args.level}")
    print(f"Episodes:   {args.n_episodes}")
    print(f"Output:     {out_path}")

    # Build env first so PPO.load can bind its observation space to the current env.
    env = SMBEnv(rom_path=args.rom, lib_path=lib_path, render_mode="rgb_array")

    try:
        model = PPO.load(args.model, env=env)
    except ValueError as e:
        print(f"\nERROR loading model: {e}")
        print("Observation space mismatch — the checkpoint was likely trained with a "
              "different version of SMBEnv. Train a fresh model with the current env.")
        env.close()
        sys.exit(1)

    best: dict = {"max_world_x": -1, "frames": []}

    try:
        for ep in range(args.n_episodes):
            result = run_episode(model, env, args.level, args.max_steps)
            status = "COMPLETE" if result["completed"] else f"x={result['max_world_x']}"
            print(f"  ep {ep + 1}/{args.n_episodes}  "
                  f"steps={result['steps']}  "
                  f"reward={result['total_reward']:.1f}  "
                  f"max_x={result['max_world_x']}  "
                  f"{status}")
            if result["max_world_x"] > best["max_world_x"]:
                best = result
    finally:
        env.close()

    if not best["frames"]:
        print("No frames captured — is the render lib built with framebuffer enabled?")
        sys.exit(1)

    with imageio.get_writer(out_path, fps=args.fps, codec="libx264", quality=8) as writer:
        for f in best["frames"]:
            writer.append_data(f)

    print(f"Saved {len(best['frames'])} frames  "
          f"max_x={best['max_world_x']}  "
          f"reward={best['total_reward']:.1f}  "
          f"→ {out_path}")


if __name__ == "__main__":
    main()
