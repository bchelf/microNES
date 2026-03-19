"""
Standalone deterministic evaluation for a trained SMBEnv PPO model.

Usage examples:

  # Eval on 1-3 only, 20 episodes, save video + CSV:
  python rl/eval_smb.py --rom roms/smb1.nes --model checkpoints/model_XXXXXXXXXX.zip \
      --levels 1-3 --n-episodes 20 --video-dir eval_videos/ --csv-out eval_metrics.csv

  # Eval on full World 1 mix:
  python rl/eval_smb.py --rom roms/smb1.nes --model checkpoints/model_XXXXXXXXXX.zip \
      --levels 1-1 1-2 1-3 1-4 --level-weights 0.15 0.15 0.5 0.2 \
      --n-episodes 40 --csv-out eval_metrics.csv

  # Metrics only (no video), frame_skip=2:
  python rl/eval_smb.py --rom roms/smb1.nes --model checkpoints/model_XXXXXXXXXX.zip \
      --levels 1-3 --n-episodes 10 --no-video --frame-skip 2 --csv-out eval_metrics.csv

Options:
  --rom PATH            SMB1 .nes ROM (required)
  --model PATH          Saved .zip model (required)
  --lib PATH            libmicrones_rl render lib (auto-detected)
  --levels LEVEL ...    Level(s) to evaluate (default: 1-3)
  --level-weights W ... Sampling weights (default: uniform)
  --n-episodes N        Episodes per level (default: 10)
  --max-steps N         Max env steps per episode (default: 5_000)
  --frame-skip N        Frame skip (default: 3; must match training)
  --platform-obs        Enable platform-aware observations (match training flag)
  --video-dir DIR       Save MP4 videos here (default: eval_videos/)
  --no-video            Skip video recording (metrics only)
  --fps N               Video frame rate (default: auto = 60 // frame_skip)
  --opening-thresholds  Level-specific opening x thresholds, e.g. 1-3:600
  --plateau-window N    Plateau detection window (default: 8; needs multiple runs)
  --csv-out PATH        Write metrics CSV to this path (default: eval_metrics.csv)
"""

from __future__ import annotations

import argparse
import csv
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))

import imageio
import numpy as np
from stable_baselines3 import PPO

from level_config import DEFAULT_OPENING_THRESHOLD, OPENING_THRESHOLDS
from smb_env import SMBEnv

# CSV columns for standalone eval output.
_CSV_FIELDS = [
    "level", "n_episodes",
    "success_rate", "max_x_mean", "max_x_p90",
    "episode_len_mean", "fall_death_rate",
    "death_x_mean", "death_x_std",
    "passed_opening_rate", "mean_reward",
]


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
    total_reward   = 0.0
    max_world_x    = 0
    frames: list[np.ndarray] = []
    passed_opening = False
    fall_death     = False
    died           = False
    completed      = False
    death_x        = None
    step_count     = 0

    last_info: dict = {}
    for _ in range(max_steps):
        action, _ = model.predict(obs, deterministic=True)
        obs, reward, terminated, truncated, info = env.step(int(action))
        total_reward += float(reward)
        step_count   += 1
        last_info     = info

        wx = info.get("world_x", 0)
        if wx > max_world_x:
            max_world_x = wx
        if not passed_opening and wx >= platform_x_thresh:
            passed_opening = True

        frame = env.render()
        if frame is not None:
            frames.append(frame)

        if terminated and obs["game_flags"][1] > 0.5:
            completed = True

        if terminated or truncated:
            break

    died       = bool(last_info.get("episode_died",       False))
    fall_death = bool(last_info.get("episode_fall_death", False))
    death_x    = last_info.get("episode_death_x", None)

    return {
        "reward":         total_reward,
        "max_world_x":    max_world_x,
        "episode_len":    step_count,
        "completed":      completed,
        "died":           died,
        "fall_death":     fall_death,
        "death_x":        death_x,
        "passed_opening": passed_opening,
        "frames":         frames,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Deterministic eval for SMBEnv PPO")
    parser.add_argument("--rom",             required=True)
    parser.add_argument("--model",           required=True, help="Path to .zip model")
    parser.add_argument("--lib",             default=None)
    parser.add_argument("--levels",          nargs="+", default=["1-3"])
    parser.add_argument("--level-weights",   nargs="+", type=float, default=None)
    parser.add_argument("--n-episodes",      type=int,   default=10,
                        help="Episodes per level")
    parser.add_argument("--max-steps",       type=int,   default=5_000)
    parser.add_argument("--frame-skip",      type=int,   default=3)
    parser.add_argument("--platform-obs",    action="store_true", default=False)
    parser.add_argument("--video-dir",       default="eval_videos")
    parser.add_argument("--no-video",        action="store_true", default=False)
    parser.add_argument("--fps",             type=int,   default=0,
                        help="Video FPS (default: auto = 60 // frame_skip)")
    parser.add_argument(
        "--opening-thresholds",
        nargs="+",
        default=None,
        metavar="LEVEL:X",
        help="Override opening x threshold per level, e.g. 1-3:600 1-1:800",
    )
    parser.add_argument(
        "--csv-out",
        default="eval_metrics.csv",
        help="Path for metrics CSV output (default: eval_metrics.csv)",
    )
    args = parser.parse_args()

    video_fps  = args.fps if args.fps > 0 else max(1, 60 // args.frame_skip)
    render_lib = None if args.no_video else _find_render_lib()

    # Resolve opening thresholds.
    opening_thresholds: dict[str, int] = dict(OPENING_THRESHOLDS)
    if args.opening_thresholds:
        for item in args.opening_thresholds:
            lvl, x = item.split(":")
            opening_thresholds[lvl.strip()] = int(x.strip())

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

    levels = args.levels

    rng = np.random.default_rng(seed=42)

    # Per-level episode lists.
    per_level_episodes: dict[str, list[dict]] = {lvl: [] for lvl in levels}
    # Best frames per level for video.
    best_frames_per_level: dict[str, list[np.ndarray]] = {lvl: [] for lvl in levels}
    best_max_x_per_level:  dict[str, int]              = {lvl: -1 for lvl in levels}

    total = args.n_episodes * len(levels)
    done  = 0
    for lvl in levels:
        thresh = opening_thresholds.get(lvl, DEFAULT_OPENING_THRESHOLD)
        for ep in range(args.n_episodes):
            result = run_episode(model, env, lvl, args.max_steps, thresh)
            per_level_episodes[lvl].append(result)

            # Track best episode for video.
            if result["max_world_x"] > best_max_x_per_level[lvl]:
                best_max_x_per_level[lvl] = result["max_world_x"]
                best_frames_per_level[lvl] = result["frames"]

            done += 1
            print(
                f"  [{lvl}] ep {ep+1}/{args.n_episodes}  "
                f"reward={result['reward']:.1f}  "
                f"max_x={result['max_world_x']}  "
                f"completed={'yes' if result['completed'] else 'no'}  "
                f"passed={'yes' if result['passed_opening'] else 'no'}  "
                f"fall={'yes' if result['fall_death'] else 'no'}  "
                f"death_x={result['death_x'] or 'n/a'}  "
                f"ep_len={result['episode_len']}"
            )

    env.close()

    # Aggregate and print summary.
    print("\n=== EVALUATION SUMMARY ===")
    csv_rows = []
    for lvl in levels:
        episodes = per_level_episodes[lvl]
        n        = len(episodes)
        thresh   = opening_thresholds.get(lvl, DEFAULT_OPENING_THRESHOLD)

        max_xs    = [ep["max_world_x"] for ep in episodes]
        ep_lens   = [ep["episode_len"]  for ep in episodes]
        rewards   = [ep["reward"]       for ep in episodes]
        death_xs  = [ep["death_x"] for ep in episodes if ep["death_x"] is not None]

        success_rate        = float(np.mean([ep["completed"]      for ep in episodes]))
        fall_death_rate     = float(np.mean([ep["fall_death"]     for ep in episodes]))
        passed_opening_rate = float(np.mean([ep["passed_opening"] for ep in episodes]))
        max_x_mean          = float(np.mean(max_xs))
        max_x_p90           = float(np.percentile(max_xs, 90))
        ep_len_mean         = float(np.mean(ep_lens))
        death_x_mean        = float(np.mean(death_xs))        if death_xs else 0.0
        death_x_std         = float(np.std(death_xs))         if len(death_xs) > 1 else 0.0
        mean_reward         = float(np.mean(rewards))

        print(f"\n  Level {lvl}  ({n} episodes)  [opening threshold x≥{thresh}]")
        print(f"    success_rate:       {success_rate:.0%}  ({sum(ep['completed'] for ep in episodes)}/{n})")
        print(f"    mean reward:        {mean_reward:.2f}")
        print(f"    max_x_mean:         {max_x_mean:.0f} ± {float(np.std(max_xs)):.0f}")
        print(f"    max_x_p90:          {max_x_p90:.0f}")
        print(f"    episode_len_mean:   {ep_len_mean:.0f} steps")
        print(f"    passed_opening:     {passed_opening_rate:.0%}  ({sum(ep['passed_opening'] for ep in episodes)}/{n})")
        print(f"    fall_death_rate:    {fall_death_rate:.0%}")
        if death_xs:
            print(f"    death_x_mean:       {death_x_mean:.0f} ± {death_x_std:.0f}")
        else:
            print(f"    death_x_mean:       n/a (no deaths)")

        csv_rows.append({
            "level":               lvl,
            "n_episodes":          n,
            "success_rate":        f"{success_rate:.4f}",
            "max_x_mean":          f"{max_x_mean:.1f}",
            "max_x_p90":           f"{max_x_p90:.1f}",
            "episode_len_mean":    f"{ep_len_mean:.1f}",
            "fall_death_rate":     f"{fall_death_rate:.4f}",
            "death_x_mean":        f"{death_x_mean:.1f}",
            "death_x_std":         f"{death_x_std:.1f}",
            "passed_opening_rate": f"{passed_opening_rate:.4f}",
            "mean_reward":         f"{mean_reward:.2f}",
        })

    # Write CSV.
    csv_path = args.csv_out
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=_CSV_FIELDS)
        writer.writeheader()
        writer.writerows(csv_rows)
    print(f"\n  Metrics CSV saved: {csv_path}")

    # Save best-episode videos.
    if not args.no_video:
        for lvl in levels:
            frames = best_frames_per_level[lvl]
            if not frames:
                continue
            tag  = lvl.replace("-", "_")
            path = os.path.join(args.video_dir, f"eval_{tag}_det.mp4")
            with imageio.get_writer(path, fps=video_fps, codec="libx264", quality=8) as w:
                for f in frames:
                    w.append_data(f)
            print(f"  Video saved (best episode): {path}")


if __name__ == "__main__":
    main()
