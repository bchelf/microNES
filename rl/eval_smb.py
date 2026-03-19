"""
Standalone deterministic evaluation for a trained SMBEnv PPO model.

Usage examples:

  # Eval on 1-3 only, 20 episodes, save video:
  python rl/eval_smb.py --rom roms/smb1.nes --model checkpoints/model_XXXXXXXXXX.zip \\
      --levels 1-3 --n-episodes 20 --video-dir eval_videos/

  # Eval on full World 1 mix:
  python rl/eval_smb.py --rom roms/smb1.nes --model checkpoints/model_XXXXXXXXXX.zip \\
      --levels 1-1 1-2 1-3 1-4 --level-weights 0.15 0.15 0.5 0.2 \\
      --n-episodes 40

  # Quick metrics only (no video), frame_skip=2:
  python rl/eval_smb.py --rom roms/smb1.nes --model checkpoints/model_XXXXXXXXXX.zip \\
      --levels 1-3 --n-episodes 10 --no-video --frame-skip 2

Options:
  --rom PATH            SMB1 .nes ROM (required)
  --model PATH          Saved .zip model (required)
  --lib PATH            libmicrones_rl render lib (auto-detected)
  --levels LEVEL ...    Level(s) to evaluate (default: 1-3)
  --level-weights W ... Sampling weights (default: uniform)
  --n-episodes N        Episodes to run per level (default: 10)
  --max-steps N         Max env steps per episode (default: 5_000)
  --frame-skip N        Frame skip (default: 3; must match training)
  --platform-obs        Enable platform-aware observations (match training flag)
  --video-dir DIR       Save MP4 videos here (default: eval_videos/)
  --no-video            Skip video recording (metrics only)
  --fps N               Video frame rate (default: 20)
  --platform-x-thresh N World-x threshold considered "passed opening" (default: 600)
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))

import imageio
import numpy as np
from stable_baselines3 import PPO

from smb_env import SMBEnv


def _find_render_lib() -> str:
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


def run_episode(
    model: PPO,
    env: SMBEnv,
    level: str,
    max_steps: int,
    platform_x_thresh: int,
) -> dict:
    """Run one deterministic episode; return metrics dict."""
    obs, _ = env.reset(options={"level": level})
    total_reward  = 0.0
    max_world_x   = 0
    frames: list[np.ndarray] = []
    passed_opening = False
    fall_deaths    = 0
    any_death      = False

    for _ in range(max_steps):
        action, _ = model.predict(obs, deterministic=True)
        obs, reward, terminated, truncated, info = env.step(int(action))
        total_reward += float(reward)

        wx = info.get("world_x", 0)
        if wx > max_world_x:
            max_world_x = wx
        if not passed_opening and wx >= platform_x_thresh:
            passed_opening = True

        frame = env.render()
        if frame is not None:
            frames.append(frame)

        if terminated:
            # Check death cause: over_void at death = fall death
            if env._cur_plat.get("over_void", False):
                fall_deaths += 1
            any_death = True

        if terminated or truncated:
            break

    return {
        "reward":          total_reward,
        "max_world_x":     max_world_x,
        "passed_opening":  passed_opening,
        "fall_deaths":     fall_deaths,
        "died":            any_death,
        "frames":          frames,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Deterministic eval for SMBEnv PPO")
    parser.add_argument("--rom",            required=True)
    parser.add_argument("--model",          required=True, help="Path to .zip model")
    parser.add_argument("--lib",            default=None)
    parser.add_argument("--levels",         nargs="+", default=["1-3"])
    parser.add_argument("--level-weights",  nargs="+", type=float, default=None)
    parser.add_argument("--n-episodes",     type=int,   default=10,
                        help="Episodes per level")
    parser.add_argument("--max-steps",      type=int,   default=5_000)
    parser.add_argument("--frame-skip",     type=int,   default=3)
    parser.add_argument("--platform-obs",   action="store_true", default=False)
    parser.add_argument("--video-dir",      default="eval_videos")
    parser.add_argument("--no-video",       action="store_true", default=False)
    parser.add_argument("--fps",            type=int,   default=0,
                        help="Video FPS (default: auto = 60 // frame_skip)")
    parser.add_argument("--platform-x-thresh", type=int, default=600,
                        help="World-x threshold for 'passed opening section' metric")
    args = parser.parse_args()

    video_fps  = args.fps if args.fps > 0 else max(1, 60 // args.frame_skip)
    render_lib = None if args.no_video else _find_render_lib()

    env = SMBEnv(
        rom_path         = args.rom,
        lib_path         = render_lib or args.lib,
        render_mode      = "rgb_array" if not args.no_video else None,
        frame_skip       = args.frame_skip,
        use_platform_obs = args.platform_obs,
        platform_shaping = 0.0,   # no shaping during eval
    )

    print(f"Loading model: {args.model}")
    model = PPO.load(args.model, env=env)

    if not args.no_video:
        os.makedirs(args.video_dir, exist_ok=True)

    # Build level list for sampling
    levels = args.levels
    weights: np.ndarray | None = None
    if args.level_weights:
        w = np.array(args.level_weights, dtype=np.float64)
        weights = w / w.sum()

    rng = np.random.default_rng(seed=42)

    # Per-level accumulators
    agg: dict[str, dict] = {lvl: {
        "rewards": [], "max_x": [], "passed": 0, "falls": 0, "deaths": 0,
    } for lvl in levels}

    all_frames_per_level: dict[str, list] = {lvl: [] for lvl in levels}

    total = args.n_episodes * len(levels)
    done  = 0
    for lvl in levels:
        for ep in range(args.n_episodes):
            result = run_episode(model, env, lvl, args.max_steps, args.platform_x_thresh)
            agg[lvl]["rewards"].append(result["reward"])
            agg[lvl]["max_x"].append(result["max_world_x"])
            if result["passed_opening"]:
                agg[lvl]["passed"] += 1
            agg[lvl]["falls"]  += result["fall_deaths"]
            if result["died"]:
                agg[lvl]["deaths"] += 1
            if result["frames"]:
                all_frames_per_level[lvl].extend(result["frames"])
            done += 1
            print(f"  [{lvl}] ep {ep+1}/{args.n_episodes}  "
                  f"reward={result['reward']:.1f}  "
                  f"max_x={result['max_world_x']}  "
                  f"passed={'yes' if result['passed_opening'] else 'no'}  "
                  f"fall_deaths={result['fall_deaths']}")

    env.close()

    # Print summary
    print("\n=== EVALUATION SUMMARY ===")
    for lvl in levels:
        g       = agg[lvl]
        n       = args.n_episodes
        rewards = g["rewards"]
        xs      = g["max_x"]
        print(f"\n  Level {lvl}  ({n} episodes)")
        print(f"    mean reward:      {np.mean(rewards):.2f} ± {np.std(rewards):.2f}")
        print(f"    mean max_world_x: {np.mean(xs):.0f} ± {np.std(xs):.0f}")
        print(f"    passed opening:   {g['passed']}/{n}  "
              f"({100*g['passed']//n}%,  x≥{args.platform_x_thresh})")
        print(f"    fall deaths:      {g['falls']}")
        print(f"    any-death eps:    {g['deaths']}/{n}")

    # Save videos
    if not args.no_video:
        for lvl in levels:
            frames = all_frames_per_level[lvl]
            if not frames:
                continue
            tag  = lvl.replace("-", "_")
            path = os.path.join(args.video_dir, f"eval_{tag}_det.mp4")
            with imageio.get_writer(path, fps=video_fps, codec="libx264", quality=8) as w:
                for f in frames:
                    w.append_data(f)
            print(f"\n  Video saved: {path}")


if __name__ == "__main__":
    main()
