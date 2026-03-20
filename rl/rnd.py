"""
Random Network Distillation (RND) for SMBEnv.

# ============================================================
# AUDIT NOTES (2026-03-19)
# ============================================================
#
# Observation space: Dict with 20 keys, total flat dim ≈ 371.
#   (computed dynamically — not hardcoded here)
#   This environment uses STRUCTURED RAM/PPU features, NOT pixels.
#   Therefore RNDNetwork uses an MLP, not a CNN.
#
# Device: auto-detected in train_smb_rl.py
#   ("mps" on Apple Silicon, "cuda" on NVIDIA, "cpu" otherwise)
#
# torch version: not pinned; follows stable-baselines3 requirement (≥1.11)
#
# RNDWrapper sits: between DeathPenaltyWrapper and SubprocVecEnv
#   Full stack (innermost → outermost):
#     SMBEnv → NewMaxXWrapper → SurvivalBonusWrapper
#     → DeathPenaltyWrapper → RNDWrapper → SubprocVecEnv → VecMonitor
#
# RNDTrainerCallback sits: FIRST in the callback list, before all others.
#   This ensures the predictor is trained on each rollout before PPO
#   updates the policy on that same rollout.
# ============================================================
"""

from __future__ import annotations

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

from running_mean_std import RunningMeanStd


# ---------------------------------------------------------------------------
# Observation flattening helpers
# ---------------------------------------------------------------------------

def flatten_obs_single(obs_dict: dict) -> np.ndarray:
    """
    Flatten a single Dict observation to a 1D float64 array.

    Keys are sorted for deterministic ordering across calls.  This same
    ordering must be used everywhere obs dicts are flattened (wrapper,
    trainer callback, etc.).
    """
    keys = sorted(obs_dict.keys())
    return np.concatenate([np.asarray(obs_dict[k], dtype=np.float64).flatten()
                           for k in keys])


def flatten_obs_batch(obs_dict: dict) -> np.ndarray:
    """
    Flatten a Dict rollout-buffer observation batch to (N, flat_dim).

    Each value in obs_dict has shape (n_steps, n_envs, *key_shape).
    Output shape: (n_steps * n_envs, flat_dim).
    """
    keys = sorted(obs_dict.keys())
    n_steps, n_envs = next(iter(obs_dict.values())).shape[:2]
    parts = [obs_dict[k].reshape(n_steps * n_envs, -1).astype(np.float32)
             for k in keys]
    return np.concatenate(parts, axis=-1)   # (N, flat_dim)


def flat_obs_dim_from_space(obs_space) -> int:
    """
    Compute total flat observation dimension from a gymnasium.spaces.Dict.
    Falls back to np.prod for non-Dict spaces.
    """
    try:
        # gymnasium.spaces.Dict
        return sum(int(np.prod(s.shape)) for s in obs_space.spaces.values())
    except AttributeError:
        return int(np.prod(obs_space.shape))


# ---------------------------------------------------------------------------
# RND network — MLP (structured feature input, not pixels)
# ---------------------------------------------------------------------------

class RNDNetwork(nn.Module):
    """
    MLP embedding network for Random Network Distillation.

    Accepts obs_shape as a tuple; computes obs_dim = prod(obs_shape) so
    the interface matches the make_rnd_networks factory regardless of
    whether obs_shape is (flat_dim,) or (C, H, W) for other envs.

    Architecture (two hidden layers of 512 units each):
        Linear(obs_dim, 512) → LeakyReLU(0.01)
        Linear(512, 512)     → LeakyReLU(0.01)
        Linear(512, embedding_dim)

    All layers use orthogonal initialisation (gain = √2 for ReLU-family).
    """

    def __init__(self, obs_shape: tuple, embedding_dim: int = 512):
        super().__init__()
        obs_dim = int(np.prod(obs_shape))

        self.net = nn.Sequential(
            nn.Linear(obs_dim, 512),
            nn.LeakyReLU(0.01),
            nn.Linear(512, 512),
            nn.LeakyReLU(0.01),
            nn.Linear(512, embedding_dim),
        )

        # Orthogonal initialisation for stability.
        for layer in self.net:
            if isinstance(layer, nn.Linear):
                nn.init.orthogonal_(layer.weight, gain=np.sqrt(2))
                nn.init.constant_(layer.bias, 0.0)

    def forward(self, obs: torch.Tensor) -> torch.Tensor:
        """
        Forward pass.  obs should be float32, already normalised to a
        reasonable range (e.g. clipped to [-5, 5] via obs_rms in the wrapper).
        Shape: (batch, obs_dim).
        """
        return self.net(obs.float())


# ---------------------------------------------------------------------------
# Factory
# ---------------------------------------------------------------------------

def make_rnd_networks(
    obs_shape: tuple,
    embedding_dim: int = 512,
    device: str = "cpu",
) -> tuple[RNDNetwork, RNDNetwork]:
    """
    Create a frozen target network and a trainable predictor network.

    Both have identical architecture; only the predictor is trained.
    The target's random, fixed weights define the novelty signal: the
    predictor must learn to match them, which it can only do for states
    it has seen many times.

    Args:
        obs_shape:     Shape of the (flattened) observation vector, e.g. (371,).
        embedding_dim: Dimensionality of the RND embedding (default 512).
        device:        torch device string.

    Returns:
        (target, predictor) — target is frozen, predictor is trainable.
    """
    target    = RNDNetwork(obs_shape, embedding_dim).to(device)
    predictor = RNDNetwork(obs_shape, embedding_dim).to(device)

    # Freeze target permanently.
    for param in target.parameters():
        param.requires_grad = False
    target.eval()

    return target, predictor


# ---------------------------------------------------------------------------
# Observation normaliser warm-up (single-env utility)
# ---------------------------------------------------------------------------

def warmup_rnd_obs_normalizer(
    rnd_wrapper,
    n_steps: int = 1000,
    verbose: bool = True,
) -> None:
    """
    Run n_steps of random actions on rnd_wrapper to prime obs_rms.

    Without warm-up, early intrinsic rewards are uncalibrated: the
    running mean and variance are at their initial values (0 and 1),
    so the normalised obs fed to the RND networks have unreliable scale
    during the first rollout.

    NOTE: when using SubprocVecEnv each worker has its own RNDWrapper
    instance with its own obs_rms.  This function warms up a single
    instance only.  For VecEnv training, use the inline warm-up loop in
    train_smb_rl.py (which steps the full VecEnv with random actions
    so all worker obs_rms instances are initialised simultaneously).

    Args:
        rnd_wrapper: An RNDWrapper instance (single env, Gymnasium API).
        n_steps:     Number of random steps to run.
        verbose:     Print progress messages.
    """
    if verbose:
        print(f"[RND] warming up obs_rms with {n_steps} random steps ...")

    obs, _info = rnd_wrapper.reset()
    for _ in range(n_steps):
        action = rnd_wrapper.action_space.sample()
        obs, _reward, terminated, truncated, _info = rnd_wrapper.step(action)
        if terminated or truncated:
            obs, _info = rnd_wrapper.reset()

    if verbose:
        rms = rnd_wrapper.obs_rms
        print(
            f"[RND] warm-up done. "
            f"obs_rms mean ∈ [{rms.mean.min():.3f}, {rms.mean.max():.3f}]  "
            f"std ∈ [{rms.std.min():.3f}, {rms.std.max():.3f}]"
        )
