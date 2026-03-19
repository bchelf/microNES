"""
Train a PPO agent on SMBEnv using Stable-Baselines3.

Usage:
    python rl/train_smb_rl.py --rom roms/smb1.nes

Options:
    --rom PATH           Path to SMB1 iNES ROM (required)
    --lib PATH           Path to libmicrones_rl.{so,dylib} (auto-detected)
    --timesteps N        Total env steps (default: 10_000_000)
    --n-envs N           Parallel envs via SubprocVecEnv (default: 8)
    --eval-interval N    Steps between eval video + checkpoint (default: 1_000_000)
    --max-eval-steps N   Max steps per eval episode (default: 5_000)
    --checkpoint-dir D   Directory for checkpoints (default: checkpoints/)
    --video-dir D        Directory for eval MP4 files (default: eval_videos/)
    --log-dir D          TensorBoard log dir (default: logs/)
    --resume PATH        Resume from a saved .zip file
"""

import argparse
import os
import sys
import time

# Allow running as `python rl/train_smb_rl.py` from repo root.
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

import numpy as np
from stable_baselines3 import PPO
from stable_baselines3.common.callbacks import BaseCallback
from stable_baselines3.common.vec_env import SubprocVecEnv, VecMonitor

from eval_callback import EvalVideoCallback
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


def make_env_fn(rom_path: str, lib_path: str | None):
    """Factory for SubprocVecEnv — runs headless, no rendering."""
    def _init():
        return SMBEnv(rom_path=rom_path, lib_path=lib_path, render_mode=None)
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
    args = parser.parse_args()

    os.makedirs(args.checkpoint_dir, exist_ok=True)
    os.makedirs(args.video_dir,      exist_ok=True)
    os.makedirs(args.log_dir,        exist_ok=True)

    print(f"ROM:            {args.rom}")
    print(f"Envs:           {args.n_envs}")
    print(f"Timesteps:      {args.timesteps:,}")
    print(f"Eval interval:  {args.eval_interval:,} steps")
    print(f"Checkpoints:    {args.checkpoint_dir}/")
    print(f"Eval videos:    {args.video_dir}/")

    # Headless training envs.
    env_fns = [make_env_fn(args.rom, args.lib) for _ in range(args.n_envs)]
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
            max_eval_steps = args.max_eval_steps,
            verbose        = 1,
        ),
        SpeedCallback(report_every=10_000),
    ]

    model.learn(
        total_timesteps     = args.timesteps,
        callback            = callbacks,
        reset_num_timesteps = args.resume is None,
        progress_bar        = True,
    )

    vec_env.close()


if __name__ == "__main__":
    main()
