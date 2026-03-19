"""
Train a PPO agent on SMBEnv using Stable-Baselines3.

Usage:
    python rl/train_smb_rl.py --rom roms/smb1.nes

    # Two-stage curriculum: 1-1/1-2 for 2M steps, then add 1-3/1-4 forever
    python rl/train_smb_rl.py --rom roms/smb1.nes \\
        --levels 1-1 1-2 --level-weights 2 1 \\
        --stage1-steps 2_000_000 \\
        --stage2-levels 1-1 1-2 1-3 1-4 --stage2-weights 4 3 2 1

Options:
    --rom PATH              Path to SMB1 iNES ROM (required)
    --lib PATH              Path to libmicrones_rl.{so,dylib} (auto-detected)
    --timesteps N           Total env steps (default: 10_000_000)
    --n-envs N              Parallel envs via SubprocVecEnv (default: 8)
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
from smb_env import SMBEnv


class SpeedCallback(BaseCallback):
    """Prints env steps/sec every `report_every` timesteps."""

    def __init__(self, report_every: int = 10_000, verbose: int = 0):
        super().__init__(verbose)
        self._report_every = report_every
        self._last_steps   = 0
        self._last_time    = time.monotonic()

    def _on_step(self) -> bool:
        n = self.num_timesteps
        if n - self._last_steps >= self._report_every:
            now     = time.monotonic()
            elapsed = now - self._last_time
            sps     = (n - self._last_steps) / elapsed if elapsed > 0 else 0
            print(f"  steps={n:>10,}  sps={sps:>8,.0f}  "
                  f"({sps / 60:.1f}x real-time per env)")
            self._last_steps = n
            self._last_time  = now
        return True


def make_env_fn(
    rom_path: str,
    lib_path: str | None,
    levels: list[str] | None = None,
    level_weights: list[float] | None = None,
):
    """Factory for SubprocVecEnv — runs headless, no rendering."""
    def _init():
        return SMBEnv(
            rom_path=rom_path,
            lib_path=lib_path,
            render_mode=None,
            levels=levels,
            level_weights=level_weights,
        )
    return _init


def main():
    parser = argparse.ArgumentParser(description="Train PPO on SMBEnv")
    parser.add_argument("--rom",             required=True,       help="Path to SMB1 .nes ROM")
    parser.add_argument("--lib",             default=None,        help="Path to libmicrones_rl")
    parser.add_argument("--timesteps",       type=int, default=10_000_000)
    parser.add_argument("--n-envs",          type=int, default=8)
    parser.add_argument("--eval-interval",   type=int, default=100_000,
                        help="Steps between eval video + checkpoint")
    parser.add_argument("--max-eval-steps",  type=int, default=5_000,
                        help="Max env steps per eval episode")
    parser.add_argument("--checkpoint-dir",  default="checkpoints")
    parser.add_argument("--video-dir",       default="eval_videos")
    parser.add_argument("--log-dir",         default="logs")
    parser.add_argument("--resume",          default=None, help="Resume from .zip")
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

    print(f"ROM:            {args.rom}")
    print(f"Envs:           {args.n_envs}")
    print(f"Timesteps:      {args.timesteps:,}")
    print(f"Stage-1 levels: {', '.join(stage1_levels)}")
    if two_stage:
        print(f"Stage-1 steps:  {args.stage1_steps:,}")
        print(f"Stage-2 levels: {', '.join(stage2_levels)}")
    print(f"Eval levels:    {', '.join(eval_levels)}")
    print(f"Eval interval:  {args.eval_interval:,} steps")
    print(f"Checkpoints:    {args.checkpoint_dir}/")
    print(f"Eval videos:    {args.video_dir}/")

    # Headless training envs (start with stage-1 curriculum).
    env_fns = [
        make_env_fn(args.rom, args.lib, levels=stage1_levels, level_weights=stage1_weights)
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
        ent_coef        = 0.01,
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

    callbacks = [
        EvalVideoCallback(
            rom_path       = args.rom,
            checkpoint_dir = args.checkpoint_dir,
            video_dir      = args.video_dir,
            eval_interval  = args.eval_interval,
            eval_levels    = eval_levels,
            max_eval_steps = args.max_eval_steps,
            verbose        = 1,
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
