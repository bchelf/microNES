"""
EvalVideoCallback — periodic checkpoint + per-level deterministic eval for SMBEnv.

Every `eval_interval` training timesteps:
  1. Saves a model checkpoint  →  checkpoints/model_{step:010d}.zip
  2. For each eval level, runs N deterministic episodes
  3. Computes per-level metrics (success_rate, max_x_mean, max_x_p90, etc.)
  4. Writes best-episode frames as MP4  →  eval_videos/eval_{level}_{step:010d}.mp4
  5. Appends a metrics row  →  checkpoints/eval_metrics.csv
  6. Logs metrics to TensorBoard
  7. Runs plateau detection; optionally stops training on persistent plateau

The eval env uses libmicrones_rl_render.dylib (framebuffer enabled).
Training envs continue using libmicrones_rl.dylib (framebuffer disabled).
"""

from __future__ import annotations

import csv
import os
from pathlib import Path
from typing import Any

import imageio
import numpy as np
from stable_baselines3.common.callbacks import BaseCallback

from level_config import DEFAULT_OPENING_THRESHOLD, OPENING_THRESHOLDS
from plateau_detector import PlateauConfig, PlateauDetector
from smb_env import SMBEnv


# CSV columns written for every eval snapshot.
_CSV_FIELDS = [
    "step", "level",
    "success_rate", "max_x_mean", "max_x_p90", "max_x_max",
    "episode_len_mean", "fall_death_rate", "death_rate",
    "death_x_mean", "death_x_std",
    "passed_opening_rate", "mean_reward",
    "plateau_flag", "plateau_reason",
]


class CurriculumStageCallback(BaseCallback):
    """
    Switches all training envs to a new set of levels at a specified step count.

    Uses SubprocVecEnv.env_method("set_curriculum", ...) so the switch happens
    in-place without restarting workers or losing model state.

    Args:
        stage2_at:      Training step count at which to switch (absolute steps).
        stage2_levels:  Level list for stage 2 (e.g. [\"1-1\",\"1-2\",\"1-3\",\"1-4\"]).
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
    Saves a checkpoint, records eval videos, computes metrics, and detects
    training plateaus every `eval_interval` training timesteps.

    Args:
        rom_path:           Path to SMB1 .nes ROM.
        checkpoint_dir:     Directory for model checkpoints and eval_metrics.csv.
        video_dir:          Directory for eval MP4 files.
        eval_interval:      Timesteps between evaluations (default 1_000_000).
        eval_levels:        List of level strings to evaluate (default ["1-1"]).
        video_fps:          Output video frame rate (default: auto = 60//frame_skip).
        max_eval_steps:     Max env steps per eval episode (default 5_000).
        render_lib_path:    Override path to libmicrones_rl_render. Auto-detected if None.
        frame_skip:         Frame skip used during training (default 3).
        use_platform_obs:   Platform observation flag (must match training).
        platform_shaping:   Platform reward shaping scale (0 = off).
        n_eval_episodes:    Deterministic episodes per level per eval (default 5).
        opening_thresholds: Dict of level→world_x threshold for passed_opening_rate.
                            Overrides level_config.OPENING_THRESHOLDS for specified levels.
        plateau_config:     PlateauConfig instance; None = use defaults.
        plateau_stop_level: If set, stop training when this level plateaus
                            for plateau_config.stop_patience consecutive windows.
        verbose:            SB3 verbosity level.
    """

    def __init__(
        self,
        rom_path: str,
        checkpoint_dir: str = "checkpoints",
        video_dir: str = "eval_videos",
        eval_interval: int = 1_000_000,
        eval_levels: list[str] | None = None,
        video_fps: int = 0,
        max_eval_steps: int = 5_000,
        render_lib_path: str | None = None,
        frame_skip: int = 3,
        use_platform_obs: bool = False,
        platform_shaping: float = 0.0,
        n_eval_episodes: int = 5,
        opening_thresholds: dict[str, int] | None = None,
        plateau_config: PlateauConfig | None = None,
        plateau_stop_level: str | None = None,
        verbose: int = 1,
    ):
        super().__init__(verbose)
        self._rom_path          = rom_path
        self._checkpoint_dir    = checkpoint_dir
        self._video_dir         = video_dir
        self._eval_interval     = eval_interval
        self._eval_levels       = eval_levels if eval_levels else ["1-1"]
        self._video_fps         = video_fps if video_fps > 0 else max(1, 60 // frame_skip)
        self._max_eval_steps    = max_eval_steps
        self._render_lib        = render_lib_path or _find_render_lib()
        self._frame_skip        = frame_skip
        self._use_platform_obs  = use_platform_obs
        self._platform_shaping  = platform_shaping
        self._n_eval_episodes   = n_eval_episodes
        self._plateau_stop_level = plateau_stop_level
        self._next_eval_at      = eval_interval

        # Merge provided thresholds on top of defaults.
        self._opening_thresholds: dict[str, int] = dict(OPENING_THRESHOLDS)
        if opening_thresholds:
            self._opening_thresholds.update(opening_thresholds)

        self._plateau = PlateauDetector(plateau_config)
        self._stop_training = False

        # CSV path initialised in _on_training_start.
        self._csv_path: str = os.path.join(checkpoint_dir, "eval_metrics.csv")

    # ------------------------------------------------------------------
    def _on_training_start(self) -> None:
        os.makedirs(self._checkpoint_dir, exist_ok=True)
        os.makedirs(self._video_dir,      exist_ok=True)

        # Initialise CSV (write header if file is new).
        self._init_csv()

        # On resume num_timesteps > 0; snap to the next eval boundary from here.
        n = self.num_timesteps
        self._next_eval_at = (n // self._eval_interval + 1) * self._eval_interval

        # Baseline: record one video + metrics per eval level before training.
        if self.verbose:
            print(f"[eval] recording baseline videos (step={n:,}) ...")
        for level in self._eval_levels:
            self._eval_level(n, level)

    def _on_step(self) -> bool:
        if self._stop_training:
            return False
        if self.num_timesteps >= self._next_eval_at:
            self._run_eval(self.num_timesteps)
            self._next_eval_at += self._eval_interval
        return not self._stop_training

    def _on_training_end(self) -> None:
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
            metrics, plateau_result = self._eval_level(step, level)

            # 3. Plateau early stop.
            if (
                self._plateau_stop_level
                and level == self._plateau_stop_level
                and plateau_result.get("should_stop", False)
            ):
                consec = plateau_result["consecutive_plateaus"]
                reason = plateau_result.get("plateau_reason", "?")
                print(
                    f"\n[plateau] STOPPING — {level} plateaued "
                    f"({consec} consecutive windows, reason={reason}). "
                    f"Final checkpoint already saved."
                )
                self._stop_training = True
                return

    # ------------------------------------------------------------------
    def _eval_level(self, step: int, level: str) -> tuple[dict, dict]:
        """
        Run N deterministic eval episodes on `level`.

        Returns (metrics_dict, plateau_result_dict).
        """
        env = SMBEnv(
            rom_path         = self._rom_path,
            lib_path         = self._render_lib,
            render_mode      = "rgb_array",
            frame_skip       = self._frame_skip,
            use_platform_obs = self._use_platform_obs,
            platform_shaping = self._platform_shaping,
        )

        episodes: list[dict] = []
        best_frames: list[np.ndarray] = []
        best_max_x = -1

        try:
            for _ in range(self._n_eval_episodes):
                ep = self._run_single_episode(env, level)
                episodes.append(ep)
                if ep["max_world_x"] > best_max_x:
                    best_max_x = ep["max_world_x"]
                    best_frames = ep["frames"]
        finally:
            env.close()

        metrics = self._aggregate_metrics(episodes, level)

        # Plateau detection.
        plateau_result = self._plateau.record(level, step, metrics)

        # Logging.
        self._log_metrics(step, level, metrics, plateau_result)

        # Save best-episode video.
        if best_frames:
            level_tag  = level.replace("-", "_")
            video_path = os.path.join(
                self._video_dir, f"eval_{level_tag}_{step:010d}.mp4"
            )
            with imageio.get_writer(
                video_path,
                fps     = self._video_fps,
                codec   = "libx264",
                quality = 8,
            ) as writer:
                for f in best_frames:
                    writer.append_data(f)
            if self.verbose:
                print(
                    f"[eval] {level}  "
                    f"max_x_mean={metrics['max_x_mean']:.1f}  "
                    f"max_x_p90={metrics['max_x_p90']:.1f}  "
                    f"max_x_max={metrics['max_x_max']:.1f}  "
                    f"death_rate={metrics['death_rate']:.2%}  "
                    f"fall_deaths={metrics['fall_death_rate']:.2%}  "
                    f"success={metrics['success_rate']:.0%}  "
                    f"passed_opening={metrics['passed_opening_rate']:.0%}  "
                    f"plateau={'YES ('+plateau_result['plateau_reason']+')' if plateau_result['plateau'] else 'no'}  "
                    f"→ {video_path}"
                )
        else:
            if self.verbose:
                print(f"[eval] {level}  no frames captured (framebuffer disabled?)")

        return metrics, plateau_result

    # ------------------------------------------------------------------
    def _run_single_episode(self, env: SMBEnv, level: str) -> dict:
        """Run one deterministic episode; return per-episode metric dict."""
        threshold = self._opening_thresholds.get(level, DEFAULT_OPENING_THRESHOLD)

        obs, _ = env.reset(options={"level": level})
        total_reward   = 0.0
        max_world_x    = 0
        frames: list[np.ndarray] = []
        completed      = False
        passed_opening = False
        step_count     = 0
        last_info: dict = {}

        for _ in range(self._max_eval_steps):
            action, _ = self.model.predict(obs, deterministic=True)
            obs, reward, terminated, truncated, info = env.step(int(action))
            total_reward += float(reward)
            step_count   += 1
            last_info     = info

            wx = info.get("world_x", 0)
            if wx > max_world_x:
                max_world_x = wx
            if not passed_opening and wx >= threshold:
                passed_opening = True

            frame = env.render()
            if frame is not None:
                frames.append(frame)

            if terminated and obs["game_flags"][1] > 0.5:
                completed = True

            if terminated or truncated:
                break

        # Read latched death state from env via final info dict.
        # These are set inside step() before the observation is returned,
        # so they are immune to the off-screen tile-clamping that masks over_void
        # at the exact death frame.
        died       = bool(last_info.get("episode_died",       False))
        fall_death = bool(last_info.get("episode_fall_death", False))
        death_x    = last_info.get("episode_death_x", None)

        # DEBUG — remove once confirmed working
        print(
            f"[death_debug] terminated={terminated} truncated={truncated} "
            f"died={died} fall_death={fall_death} death_x={death_x}"
        )

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

    # ------------------------------------------------------------------
    def _aggregate_metrics(self, episodes: list[dict], level: str) -> dict:
        """Aggregate per-episode results into level-summary metrics."""
        max_xs    = [ep["max_world_x"]  for ep in episodes]
        ep_lens   = [ep["episode_len"]  for ep in episodes]
        rewards   = [ep["reward"]       for ep in episodes]
        fall_xs   = [ep["death_x"] for ep in episodes if ep["death_x"] is not None]

        success_rate        = float(np.mean([ep["completed"]      for ep in episodes]))
        fall_death_rate     = float(np.mean([ep["fall_death"]     for ep in episodes]))
        passed_opening_rate = float(np.mean([ep["passed_opening"] for ep in episodes]))

        death_rate = float(np.mean([ep["died"] for ep in episodes]))

        return {
            "success_rate":        success_rate,
            "max_x_mean":          float(np.mean(max_xs)),
            "max_x_p90":           float(np.percentile(max_xs, 90)),
            "max_x_max":           float(np.max(max_xs)),
            "episode_len_mean":    float(np.mean(ep_lens)),
            "fall_death_rate":     fall_death_rate,
            "death_rate":          death_rate,
            "death_x_mean":        float(np.mean(fall_xs))         if fall_xs else 0.0,
            "death_x_std":         float(np.std(fall_xs))          if len(fall_xs) > 1 else 0.0,
            "passed_opening_rate": passed_opening_rate,
            "mean_reward":         float(np.mean(rewards)),
        }

    # ------------------------------------------------------------------
    def _init_csv(self) -> None:
        """Write CSV header if the file does not already exist."""
        if not os.path.exists(self._csv_path):
            with open(self._csv_path, "w", newline="") as f:
                csv.DictWriter(f, fieldnames=_CSV_FIELDS).writeheader()

    def _log_metrics(
        self,
        step: int,
        level: str,
        metrics: dict,
        plateau_result: dict,
    ) -> None:
        """Append row to CSV and emit to TensorBoard."""
        # CSV.
        row = {
            "step":                 step,
            "level":                level,
            "success_rate":         f"{metrics['success_rate']:.4f}",
            "max_x_mean":           f"{metrics['max_x_mean']:.1f}",
            "max_x_p90":            f"{metrics['max_x_p90']:.1f}",
            "max_x_max":            f"{metrics['max_x_max']:.1f}",
            "episode_len_mean":     f"{metrics['episode_len_mean']:.1f}",
            "fall_death_rate":      f"{metrics['fall_death_rate']:.4f}",
            "death_rate":           f"{metrics['death_rate']:.4f}",
            "death_x_mean":         f"{metrics['death_x_mean']:.1f}",
            "death_x_std":          f"{metrics['death_x_std']:.1f}",
            "passed_opening_rate":  f"{metrics['passed_opening_rate']:.4f}",
            "mean_reward":          f"{metrics['mean_reward']:.2f}",
            "plateau_flag":         int(plateau_result.get("plateau", False)),
            "plateau_reason":       plateau_result.get("plateau_reason") or "",
        }
        with open(self._csv_path, "a", newline="") as f:
            csv.DictWriter(f, fieldnames=_CSV_FIELDS).writerow(row)

        # TensorBoard (best-effort — logger may not be available at baseline eval).
        try:
            prefix = f"eval/{level}"
            for key, val in metrics.items():
                self.logger.record(f"{prefix}/{key}", float(val))
            self.logger.record(
                f"{prefix}/plateau",
                int(plateau_result.get("plateau", False)),
            )
            self.logger.dump(step)
        except Exception:
            pass
