"""
EvalVideoCallback — periodic checkpoint + per-level deterministic eval for SMBEnv.

Every `eval_interval` training timesteps:
  1. Saves a model checkpoint  →  checkpoints/model_{step:010d}.zip
  2. For each eval level, runs one deterministic episode with render_mode="rgb_array"
  3. Writes collected frames as MP4  →  eval_videos/eval_{level}_{step:010d}.mp4

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


class CurriculumStageCallback(BaseCallback):
    """
    Switches all training envs to a new set of levels at a specified step count.

    Uses SubprocVecEnv.env_method("set_curriculum", ...) so the switch happens
    in-place without restarting workers or losing model state.

    Args:
        stage2_at:      Training step count at which to switch (absolute steps).
        stage2_levels:  Level list for stage 2 (e.g. ["1-1","1-2","1-3","1-4"]).
        stage2_weights: Optional sampling weights (unnormalized). Default: uniform.
        verbose:        Print a message when the switch fires.
    """

    def __init__(
        self,
        stage2_at: int,
        stage2_levels: list[str],
        stage2_weights: list[float] | None = None,
        verbose: int = 1,
    ):
        super().__init__(verbose)
        self._stage2_at      = stage2_at
        self._stage2_levels  = stage2_levels
        self._stage2_weights = stage2_weights
        self._switched       = False

    def _on_step(self) -> bool:
        if not self._switched and self.num_timesteps >= self._stage2_at:
            self.training_env.env_method(
                "set_curriculum",
                self._stage2_levels,
                self._stage2_weights,
            )
            self._switched = True
            if self.verbose:
                wt = self._stage2_weights or ["uniform"] * len(self._stage2_levels)
                pairs = ", ".join(
                    f"{l}({w})" for l, w in zip(self._stage2_levels, wt)
                )
                print(
                    f"\n[curriculum] step={self.num_timesteps:,} → stage 2: {pairs}"
                )
        return True


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
    Saves a checkpoint and records one deterministic eval episode per level as MP4
    every `eval_interval` training timesteps.

    Args:
        rom_path:        Path to SMB1 .nes ROM.
        checkpoint_dir:  Directory for model checkpoints.
        video_dir:       Directory for eval MP4 files.
        eval_interval:   Timesteps between evaluations (default 1_000_000).
        eval_levels:     List of level strings to evaluate (default ["1-1"]).
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
        eval_levels: list[str] | None = None,
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
        self._eval_levels    = eval_levels if eval_levels else ["1-1"]
        self._video_fps      = video_fps
        self._max_eval_steps = max_eval_steps
        self._render_lib     = render_lib_path or _find_render_lib()
        self._next_eval_at   = eval_interval

    def _on_training_start(self) -> None:
        os.makedirs(self._checkpoint_dir, exist_ok=True)
        os.makedirs(self._video_dir,      exist_ok=True)
        # On resume num_timesteps > 0; snap to the next eval boundary from here.
        n = self.num_timesteps
        self._next_eval_at = (n // self._eval_interval + 1) * self._eval_interval
        # Baseline: record one video per eval level before any training begins.
        if self.verbose:
            print(f"[eval] recording baseline videos (step={n:,}) ...")
        for level in self._eval_levels:
            self._eval_level(n, level)

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

        # 2. Evaluate each level.
        for level in self._eval_levels:
            self._eval_level(step, level)

    def _eval_level(self, step: int, level: str) -> None:
        """Run one deterministic episode on `level` and write an MP4."""
        env = SMBEnv(
            rom_path    = self._rom_path,
            lib_path    = self._render_lib,
            render_mode = "rgb_array",
        )

        frames: list[np.ndarray] = []
        total_reward = 0.0

        try:
            obs, _ = env.reset(options={"level": level})
            for _ in range(self._max_eval_steps):
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
        level_tag = level.replace("-", "_")
        video_path = os.path.join(self._video_dir, f"eval_{level_tag}_{step:010d}.mp4")
        if frames:
            with imageio.get_writer(
                video_path,
                fps     = self._video_fps,
                codec   = "libx264",
                quality = 8,
            ) as writer:
                for f in frames:
                    writer.append_data(f)
            if self.verbose:
                print(f"[eval] {level}  {len(frames)} frames  "
                      f"reward={total_reward:.1f}  → {video_path}")
        else:
            if self.verbose:
                print(f"[eval] {level}  no frames captured (framebuffer disabled?)")
