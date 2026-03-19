"""
EvalVideoCallback — periodic checkpoint + deterministic eval video for SMBEnv.

Every `eval_interval` training timesteps:
  1. Saves a model checkpoint  →  checkpoints/model_{step:010d}.zip
  2. Runs one deterministic eval episode with render_mode="rgb_array"
  3. Writes collected frames as MP4  →  eval_videos/eval_{step:010d}.mp4

The eval env uses libmicrones_rl_render.dylib (framebuffer enabled).
Training envs continue using libmicrones_rl.dylib (framebuffer disabled).
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any

import imageio
import numpy as np
from stable_baselines3.common.callbacks import BaseCallback

from smb_env import SMBEnv


def _find_render_lib() -> str:
    """Find libmicrones_rl_render.{dylib,so} next to the fast training lib."""
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


class EvalVideoCallback(BaseCallback):
    """
    Saves a checkpoint and records one deterministic eval episode as MP4
    every `eval_interval` training timesteps.

    Args:
        rom_path:        Path to SMB1 .nes ROM.
        checkpoint_dir:  Directory for model checkpoints.
        video_dir:       Directory for eval MP4 files.
        eval_interval:   Timesteps between evaluations (default 1_000_000).
        video_fps:       Output video frame rate (default 20 — matches frame_skip=3).
        max_eval_steps:  Max env steps per eval episode (default 5_000).
        render_lib_path: Override path to libmicrones_rl_render. Auto-detected if None.
        verbose:         SB3 verbosity level.
    """

    def __init__(
        self,
        rom_path: str,
        checkpoint_dir: str = "checkpoints",
        video_dir: str = "eval_videos",
        eval_interval: int = 1_000_000,
        video_fps: int = 20,
        max_eval_steps: int = 5_000,
        render_lib_path: str | None = None,
        verbose: int = 1,
    ):
        super().__init__(verbose)
        self._rom_path       = rom_path
        self._checkpoint_dir = checkpoint_dir
        self._video_dir      = video_dir
        self._eval_interval  = eval_interval
        self._video_fps      = video_fps
        self._max_eval_steps = max_eval_steps
        self._render_lib     = render_lib_path or _find_render_lib()
        self._next_eval_at   = eval_interval

    def _on_training_start(self) -> None:
        os.makedirs(self._checkpoint_dir, exist_ok=True)
        os.makedirs(self._video_dir,      exist_ok=True)

    def _on_step(self) -> bool:
        if self.num_timesteps >= self._next_eval_at:
            self._run_eval(self.num_timesteps)
            self._next_eval_at += self._eval_interval
        return True

    def _on_training_end(self) -> None:
        # Always save a final checkpoint when training finishes.
        step = self.num_timesteps
        path = os.path.join(self._checkpoint_dir, f"model_{step:010d}_final")
        self.model.save(path)
        if self.verbose:
            print(f"[eval] final model saved → {path}.zip")

    # ------------------------------------------------------------------
    def _run_eval(self, step: int) -> None:
        # 1. Save checkpoint.
        ckpt_path = os.path.join(self._checkpoint_dir, f"model_{step:010d}")
        self.model.save(ckpt_path)
        if self.verbose:
            print(f"\n[eval] step={step:,}  checkpoint → {ckpt_path}.zip")

        # 2. Create a single rendering env.
        env = SMBEnv(
            rom_path    = self._rom_path,
            lib_path    = self._render_lib,
            render_mode = "rgb_array",
        )

        frames: list[np.ndarray] = []
        total_reward = 0.0

        try:
            obs, _ = env.reset()
            for _ in range(self._max_eval_steps):
                # Deterministic action from current model.
                action, _ = self.model.predict(obs, deterministic=True)
                obs, reward, terminated, truncated, _ = env.step(int(action))
                total_reward += float(reward)

                frame = env.render()
                if frame is not None:
                    frames.append(frame)

                if terminated or truncated:
                    break
        finally:
            env.close()

        # 3. Write MP4.
        if frames:
            video_path = os.path.join(self._video_dir, f"eval_{step:010d}.mp4")
            with imageio.get_writer(
                video_path,
                fps    = self._video_fps,
                codec  = "libx264",
                quality= 8,
            ) as writer:
                for f in frames:
                    writer.append_data(f)
            if self.verbose:
                print(f"[eval] {len(frames)} frames, reward={total_reward:.1f}"
                      f"  video → {video_path}")
        else:
            if self.verbose:
                print(f"[eval] no frames captured (framebuffer disabled?)")
