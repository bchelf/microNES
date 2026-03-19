"""
Train a PPO agent on SMBEnv using Stable-Baselines3.

Usage:
    python rl/train_smb_rl.py --rom roms/smb1.nes

    # 1-3-focused World 1 curriculum (resume-compatible, no obs change):
    python rl/train_smb_rl.py --rom roms/smb1.nes \\
        --levels 1-1 1-2 1-3 1-4 --level-weights 0.15 0.15 0.5 0.2 \\
        --resume checkpoints/model_XXXXXXXXXX.zip \\
        --checkpoint-dir checkpoints_13/ --video-dir eval_videos_13/

    # Same with new platform observations (requires fresh start):
    python rl/train_smb_rl.py --rom roms/smb1.nes \\
        --levels 1-1 1-2 1-3 1-4 --level-weights 0.15 0.15 0.5 0.2 \\
        --platform-obs --frame-skip 2 \\
        --checkpoint-dir checkpoints_13_fs2/ --video-dir eval_videos_13_fs2/

Options:
    --rom PATH              Path to SMB1 iNES ROM (required)
    --lib PATH              Path to libmicrones_rl.{so,dylib} (auto-detected)
    --timesteps N           Total env steps (default: 10_000_000)
    --n-envs N              Parallel envs via SubprocVecEnv (default: 8)
    --frame-skip N          NES frames to skip per step (default: 3)
    --eval-interval N       Steps between eval video + checkpoint (default: 100_000)
    --max-eval-steps N      Max steps per eval episode (default: 5_000)
    --checkpoint-dir D      Directory for checkpoints (default: checkpoints/)
    --video-dir D           Directory for eval MP4 files (default: eval_videos/)
    --log-dir D             TensorBoard log dir (default: logs/)
    --resume PATH           Resume from a saved .zip file
    --levels LEVEL ...      Stage-1 curriculum levels (default: 1-1)
    --level-weights W ...   Sampling weights for --levels (default: uniform)
    --stage1-steps N        Switch to stage 2 after N steps (omit for single stage)
    --stage2-levels LEVEL . Stage-2 level list (required if --stage1-steps set)
    --stage2-weights W ...  Sampling weights for --stage2-levels (default: uniform)
    --eval-levels LEVEL ... Levels to record eval videos for (default: all stage levels)
    --platform-obs          Add platform-aware obs features (default: off)
    --platform-shaping F    Scale for platform reward shaping, 0=disable (default: 1.0)
"""

import argparse
import json
import os
import sys
import time

# Allow running as `python rl/train_smb_rl.py` from repo root.
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

import numpy as np
from stable_baselines3 import PPO
from stable_baselines3.common.callbacks import BaseCallback
from stable_baselines3.common.vec_env import SubprocVecEnv, VecMonitor

from eval_callback import CurriculumStageCallback, EvalVideoCallback
from plateau_detector import PlateauConfig
from smb_env import SMBEnv


class SpeedCallback(BaseCallback):
    """Prints env steps/sec and rolling max_x every `report_every` timesteps."""

    def __init__(self, report_every: int = 10_000, verbose: int = 0):
        super().__init__(verbose)
        self._report_every  = report_every
        self._last_steps    = 0
        self._last_time     = time.monotonic()
        self._window_max_x  = 0   # max world_x seen in current reporting window
        self._alltime_max_x = 0   # max world_x seen across entire training run

    def _on_step(self) -> bool:
        # Track max world_x from all envs this step.
        for info in self.locals.get("infos", []):
            wx = info.get("world_x", 0)
            if wx > self._window_max_x:
                self._window_max_x = wx
            if wx > self._alltime_max_x:
                self._alltime_max_x = wx

        n = self.num_timesteps
        if n - self._last_steps >= self._report_every:
            now     = time.monotonic()
            elapsed = now - self._last_time
            sps     = (n - self._last_steps) / elapsed if elapsed > 0 else 0
            print(f"  steps={n:>10,}  sps={sps:>8,.0f}  "
                  f"({sps / 60:.1f}x real-time per env)  "
                  f"max_x_window={self._window_max_x}  max_x_alltime={self._alltime_max_x}")
            self._last_steps   = n
            self._last_time    = now
            self._window_max_x = 0   # reset window for next interval
        return True


def make_env_fn(
    rom_path: str,
    lib_path: str | None,
    levels: list[str] | None = None,
    level_weights: list[float] | None = None,
    frame_skip: int = 3,
    use_platform_obs: bool = False,
    platform_shaping: float = 1.0,
):
    """Factory for SubprocVecEnv — runs headless, no rendering."""
    def _init():
        return SMBEnv(
            rom_path=rom_path,
            lib_path=lib_path,
            render_mode=None,
            frame_skip=frame_skip,
            levels=levels,
            level_weights=level_weights,
            use_platform_obs=use_platform_obs,
            platform_shaping=platform_shaping,
        )
    return _init


def main():
    parser = argparse.ArgumentParser(description="Train PPO on SMBEnv")
    parser.add_argument("--rom",             required=True,       help="Path to SMB1 .nes ROM")
    parser.add_argument("--lib",             default=None,        help="Path to libmicrones_rl")
    parser.add_argument("--timesteps",       type=int, default=10_000_000)
    parser.add_argument("--n-envs",          type=int, default=8)
    parser.add_argument("--frame-skip",      type=int, default=3,
                        help="NES frames to advance per env step (default: 3)")
    parser.add_argument("--eval-interval",   type=int, default=100_000,
                        help="Steps between eval video + checkpoint")
    parser.add_argument("--max-eval-steps",  type=int, default=5_000,
                        help="Max env steps per eval episode")
    parser.add_argument("--checkpoint-dir",  default="checkpoints")
    parser.add_argument("--video-dir",       default="eval_videos")
    parser.add_argument("--log-dir",         default="logs")
    parser.add_argument("--resume",          default=None, help="Resume from .zip")
    parser.add_argument("--platform-obs",    action="store_true", default=False,
                        help="Add platform-aware observation features (breaks ckpt compat)")
    parser.add_argument("--platform-shaping", type=float, default=1.0,
                        help="Scale for platform reward shaping terms (0=disabled)")
    parser.add_argument(
        "--levels",
        nargs="+",
        default=None,
        metavar="LEVEL",
        help="Curriculum levels to sample from (e.g. 1-1 1-2 1-3 1-4). Default: 1-1 only.",
    )
    parser.add_argument(
        "--level-weights",
        nargs="+",
        type=float,
        default=None,
        metavar="W",
        help="Sampling weights for --levels (unnormalized). Default: uniform.",
    )
    parser.add_argument(
        "--eval-levels",
        nargs="+",
        default=None,
        metavar="LEVEL",
        help="Levels to record eval videos for. Defaults to union of all stage levels.",
    )
    parser.add_argument(
        "--stage1-steps",
        type=int,
        default=None,
        metavar="N",
        help="Switch from stage-1 to stage-2 curriculum after N training steps.",
    )
    parser.add_argument(
        "--stage2-levels",
        nargs="+",
        default=None,
        metavar="LEVEL",
        help="Stage-2 level list (required when --stage1-steps is set).",
    )
    parser.add_argument(
        "--stage2-weights",
        nargs="+",
        type=float,
        default=None,
        metavar="W",
        help="Sampling weights for --stage2-levels (default: uniform).",
    )
    # ---- Eval / plateau detection ----
    parser.add_argument(
        "--eval-episodes",
        type=int,
        default=5,
        metavar="N",
        help="Deterministic eval episodes per level per checkpoint (default: 5).",
    )
    parser.add_argument(
        "--opening-thresholds",
        nargs="+",
        default=None,
        metavar="LEVEL:X",
        help="Override opening section x threshold per level, e.g. 1-3:600 1-1:800.",
    )
    parser.add_argument(
        "--plateau-window",
        type=int,
        default=8,
        metavar="N",
        help="Rolling window of eval snapshots for plateau detection (default: 8).",
    )
    parser.add_argument(
        "--plateau-max-x-gain",
        type=float,
        default=50.0,
        metavar="F",
        help="Min max_x_mean gain over plateau window to avoid plateau flag (default: 50).",
    )
    parser.add_argument(
        "--plateau-passed-opening-gain",
        type=float,
        default=0.05,
        metavar="F",
        help="Min passed_opening_rate gain over window (default: 0.05).",
    )
    parser.add_argument(
        "--plateau-success-gain",
        type=float,
        default=0.02,
        metavar="F",
        help="Min success_rate gain over window (default: 0.02).",
    )
    parser.add_argument(
        "--plateau-stop-level",
        default=None,
        metavar="LEVEL",
        help="Stop training when this level plateaus for --plateau-stop-patience windows.",
    )
    parser.add_argument(
        "--plateau-stop-patience",
        type=int,
        default=0,
        metavar="N",
        help="Consecutive plateau windows before stopping (0 = disabled, default: 0).",
    )
    parser.add_argument(
        "--ent-coef",
        type=float,
        default=0.01,
        metavar="F",
        help="PPO entropy coefficient (default: 0.01; try 0.02-0.05 to combat entropy collapse).",
    )
    args = parser.parse_args()

    # ---- Validate and resolve curriculum settings ----
    stage1_levels  = args.levels or ["1-1"]
    stage1_weights = args.level_weights
    two_stage      = args.stage1_steps is not None

    if two_stage and not args.stage2_levels:
        parser.error("--stage2-levels is required when --stage1-steps is set")
    if stage1_weights and len(stage1_weights) != len(stage1_levels):
        parser.error("--level-weights must have the same length as --levels")
    if args.stage2_weights and args.stage2_levels and \
            len(args.stage2_weights) != len(args.stage2_levels):
        parser.error("--stage2-weights must have the same length as --stage2-levels")

    stage2_levels  = args.stage2_levels or []
    stage2_weights = args.stage2_weights

    # Eval levels default to union of all stage levels (preserving order).
    if args.eval_levels:
        eval_levels = args.eval_levels
    else:
        seen: dict[str, None] = {}
        for lvl in stage1_levels + stage2_levels:
            seen[lvl] = None
        eval_levels = list(seen.keys())

    os.makedirs(args.checkpoint_dir, exist_ok=True)
    os.makedirs(args.video_dir,      exist_ok=True)
    os.makedirs(args.log_dir,        exist_ok=True)

    # Save curriculum metadata alongside checkpoints.
    curriculum_meta: dict = {
        "stage1_levels":  stage1_levels,
        "stage1_weights": stage1_weights,
        "eval_levels":    eval_levels,
    }
    if two_stage:
        curriculum_meta["stage1_steps"]  = args.stage1_steps
        curriculum_meta["stage2_levels"] = stage2_levels
        curriculum_meta["stage2_weights"]= stage2_weights
    with open(os.path.join(args.checkpoint_dir, "curriculum.json"), "w") as f:
        json.dump(curriculum_meta, f, indent=2)

    print(f"ROM:              {args.rom}")
    print(f"Envs:             {args.n_envs}")
    print(f"Frame skip:       {args.frame_skip}")
    print(f"Timesteps:        {args.timesteps:,}")
    print(f"Platform obs:     {args.platform_obs}")
    print(f"Platform shaping: {args.platform_shaping}")
    print(f"Stage-1 levels:   {', '.join(stage1_levels)}")
    if stage1_weights:
        print(f"Stage-1 weights:  {stage1_weights}")
    if two_stage:
        print(f"Stage-1 steps:  {args.stage1_steps:,}")
        print(f"Stage-2 levels: {', '.join(stage2_levels)}")
    print(f"Eval levels:    {', '.join(eval_levels)}")
    print(f"Eval interval:  {args.eval_interval:,} steps")
    print(f"Eval episodes:  {args.eval_episodes} per level")
    print(f"Checkpoints:    {args.checkpoint_dir}/")
    print(f"Eval videos:    {args.video_dir}/")
    print(f"Plateau window: {args.plateau_window}  "
          f"max_x_gain={args.plateau_max_x_gain}  "
          f"passed_gain={args.plateau_passed_opening_gain}  "
          f"success_gain={args.plateau_success_gain}")
    if args.plateau_stop_level:
        print(f"Plateau stop:   {args.plateau_stop_level} "
              f"after {args.plateau_stop_patience} consecutive windows")

    # Save frame_skip and obs config alongside curriculum metadata.
    curriculum_meta["frame_skip"]        = args.frame_skip
    curriculum_meta["platform_obs"]      = args.platform_obs
    curriculum_meta["platform_shaping"]  = args.platform_shaping

    # Headless training envs (start with stage-1 curriculum).
    env_fns = [
        make_env_fn(
            args.rom, args.lib,
            levels=stage1_levels,
            level_weights=stage1_weights,
            frame_skip=args.frame_skip,
            use_platform_obs=args.platform_obs,
            platform_shaping=args.platform_shaping,
        )
        for _ in range(args.n_envs)
    ]
    vec_env = SubprocVecEnv(env_fns)
    vec_env = VecMonitor(vec_env, filename=os.path.join(args.log_dir, "monitor"))

    ppo_kwargs = dict(
        policy          = "MultiInputPolicy",
        env             = vec_env,
        n_steps         = 512,
        batch_size      = 256,
        n_epochs        = 4,
        learning_rate   = 3e-4,
        gamma           = 0.99,
        gae_lambda      = 0.95,
        clip_range      = 0.2,
        ent_coef        = args.ent_coef,
        vf_coef         = 0.5,
        max_grad_norm   = 0.5,
        tensorboard_log = args.log_dir,
        verbose         = 1,
        policy_kwargs   = dict(
            net_arch = dict(pi=[256, 256], vf=[256, 256]),
        ),
    )

    if args.resume:
        print(f"Resuming from {args.resume}")
        model = PPO.load(args.resume, env=vec_env, **{
            k: v for k, v in ppo_kwargs.items()
            if k not in ("policy", "env")
        })
    else:
        model = PPO(**ppo_kwargs)

    # Parse opening thresholds overrides.
    opening_thresholds_override: dict[str, int] = {}
    if args.opening_thresholds:
        for item in args.opening_thresholds:
            lvl, x = item.split(":")
            opening_thresholds_override[lvl.strip()] = int(x.strip())

    plateau_cfg = PlateauConfig(
        window                    = args.plateau_window,
        min_max_x_gain            = args.plateau_max_x_gain,
        min_passed_opening_gain   = args.plateau_passed_opening_gain,
        min_success_rate_gain     = args.plateau_success_gain,
        stop_patience             = args.plateau_stop_patience,
    )

    callbacks = [
        EvalVideoCallback(
            rom_path            = args.rom,
            checkpoint_dir      = args.checkpoint_dir,
            video_dir           = args.video_dir,
            eval_interval       = args.eval_interval,
            eval_levels         = eval_levels,
            max_eval_steps      = args.max_eval_steps,
            frame_skip          = args.frame_skip,
            use_platform_obs    = args.platform_obs,
            platform_shaping    = args.platform_shaping,
            n_eval_episodes     = args.eval_episodes,
            opening_thresholds  = opening_thresholds_override or None,
            plateau_config      = plateau_cfg,
            plateau_stop_level  = args.plateau_stop_level,
            verbose             = 1,
        ),
        SpeedCallback(report_every=10_000),
    ]

    if two_stage:
        callbacks.append(CurriculumStageCallback(
            stage2_at      = args.stage1_steps,
            stage2_levels  = stage2_levels,
            stage2_weights = stage2_weights,
            verbose        = 1,
        ))

    model.learn(
        total_timesteps     = args.timesteps,
        callback            = callbacks,
        reset_num_timesteps = args.resume is None,
        progress_bar        = True,
    )

    vec_env.close()


if __name__ == "__main__":
    main()
