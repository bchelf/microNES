"""
Train a PPO agent on SMBEnv using Stable-Baselines3.

Usage:
    python rl/train_smb_rl.py --rom roms/smb1.nes

    # With RND intrinsic motivation (default: enabled):
    python rl/train_smb_rl.py --rom roms/smb1.nes --rnd-scale 1.0

    # Disable RND:
    python rl/train_smb_rl.py --rom roms/smb1.nes --no-rnd

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
    --no-rnd                Disable RND intrinsic motivation (default: RND enabled)
    --rnd-scale FLOAT       Intrinsic reward multiplier (default: 1.0)
    --rnd-warmup-steps N    Random steps to prime obs_rms before training (default: 1000)
"""

import argparse
import json
import os
import sys
import time

# Allow running as `python rl/train_smb_rl.py` from repo root.
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

import numpy as np
import torch
from stable_baselines3 import PPO
from stable_baselines3.common.callbacks import BaseCallback
from stable_baselines3.common.vec_env import SubprocVecEnv, VecMonitor

from eval_callback import (
    BestRunRecorderCallback,
    CurriculumStageCallback,
    DiagnosticsCallback,
    EntropySchedulerCallback,
    EvalVideoCallback,
    RNDTrainerCallback,
)
from rnd import flat_obs_dim_from_space, make_rnd_networks
from smb_env import SMBEnv
from wrappers import (
    AirborneActionMaskWrapper,
    DeathPenaltyWrapper,
    NewMaxXWrapper,
    PlatformClimbRewardWrapper,
    RNDWrapper,
    StickyActionWrapper,
    StompRewardWrapper,
    SurvivalBonusWrapper,
    VisitedCellsWrapper,
)


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
    rnd_obs_shape: tuple | None = None,
    rnd_embedding_dim: int = 512,
    rnd_scale: float = 1.0,
    cell_bonus: float = 1.0,
    sticky_prob: float = 0.25,
    stomp_bonus: float = 5.0,
    climb_bonus: float = 2.0,
    use_sticky: bool = True,
    use_stomp: bool = True,
    use_climb: bool = True,
):
    """
    Factory for SubprocVecEnv — runs headless, no rendering.

    Wrapper stack (innermost → outermost):
      SMBEnv
        → AirborneActionMaskWrapper                     replaces jump actions with WAIT while airborne
        → StickyActionWrapper(sticky_prob=0.25)         action repeat — helps sustained jumps
        → NewMaxXWrapper(scale=2.0, active=False)       diagnostic only — no reward bonus
        → SurvivalBonusWrapper(bonus=0.02)              per-step alive bonus
        → DeathPenaltyWrapper(penalty=4.0)              additive death penalty
        → StompRewardWrapper(stomp_bonus=5.0)           +5 for stomping Goomba/Koopa/Beetle
        → PlatformClimbRewardWrapper(climb_bonus=2.0)   +2 for landing forward+higher
        → VisitedCellsWrapper(cell_bonus=1.0)           2D cell exploration bonus
        → RNDWrapper(scale=rnd_scale)                   intrinsic curiosity (when enabled)

    StickyActionWrapper is innermost (closest to SMBEnv) so it intercepts actions
    before any reward wrapper sees them.

    StompRewardWrapper sits between DeathPenaltyWrapper and VisitedCellsWrapper —
    after death detection (so stomp bonus doesn't interact with death penalty) and
    before cell bonus (so a stomp that crosses into a new cell rewards both).

    NewMaxXWrapper is kept with active=False so that max_x_seen remains trackable
    via get_attr for EntropySchedulerCallback plateau detection, without adding
    any 1D frontier reward.  VisitedCellsWrapper replaces the frontier signal with
    a 2D (world_x, screen_y) tile-cell exploration bonus that incentivizes both
    horizontal progress AND vertical platform exploration.

    RNDWrapper is the outermost single-env wrapper.  RND networks are created
    INSIDE each subprocess worker (always on CPU) to avoid MPS + forkserver
    incompatibility.  On macOS, SB3's SubprocVecEnv uses forkserver, which
    forks workers from a clean server process where MPS is never initialized;
    passing MPS tensors through the closure causes silent worker crashes.

    After SubprocVecEnv is constructed, train_smb_rl.py syncs the main-process
    target and predictor weights to all workers via env_method so that all
    workers start from the same network state as the main process.

    The main-process networks (on MPS/CUDA/CPU) are trained by
    RNDTrainerCallback, which then syncs predictor weights to subprocesses
    via env_method("sync_rnd_predictor") after each rollout.
    """
    use_rnd = rnd_obs_shape is not None

    def _init():
        env = SMBEnv(
            rom_path=rom_path,
            lib_path=lib_path,
            render_mode=None,
            levels=levels,
            level_weights=level_weights,
        )
        env = AirborneActionMaskWrapper(env)
        if use_sticky:
            env = StickyActionWrapper(env, sticky_prob=sticky_prob)
        env = NewMaxXWrapper(env, scale=2.0, active=True)
        env = SurvivalBonusWrapper(env)
        env = DeathPenaltyWrapper(env)
        if use_stomp:
            env = StompRewardWrapper(env, stomp_bonus=stomp_bonus)
        if use_climb:
            env = PlatformClimbRewardWrapper(env, climb_bonus=climb_bonus)
        env = VisitedCellsWrapper(env, cell_size_x=8, cell_size_y=8,
                                  cell_bonus=cell_bonus)
        if use_rnd:
            # Create networks fresh inside the worker, always on CPU.
            # Weights are overwritten immediately by the initial sync in main().
            _target, _predictor = make_rnd_networks(
                obs_shape     = rnd_obs_shape,
                embedding_dim = rnd_embedding_dim,
                device        = "cpu",
            )
            env = RNDWrapper(
                env,
                rnd_target      = _target,
                rnd_predictor   = _predictor,
                intrinsic_scale = rnd_scale,
                device          = "cpu",
            )
        return env
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
    parser.add_argument("--best-run-dir",    default="best_runs",
                        help="Directory for best-episode .npy + MP4 files (default: best_runs/)")
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
    # ---- VisitedCells arguments ----
    parser.add_argument(
        "--cell-bonus",
        type=float,
        default=1.0,
        metavar="FLOAT",
        help=(
            "Reward per new (world_x, screen_y) tile cell visited while on ground "
            "(default: 1.0). Monitor diagnostics/cells_per_step:\n"
            "  too fast (>0.1) → increase cell_size or reduce --cell-bonus\n"
            "  stagnant (0.0)  → reduce cell_size or increase --cell-bonus\n"
            "  0.01-0.05       → healthy exploration rate"
        ),
    )
    # ---- StickyAction arguments ----
    parser.add_argument(
        "--sticky-prob",
        type=float,
        default=0.25,
        metavar="FLOAT",
        help="Probability of repeating previous action (default: 0.25). "
             "Range 0.0–0.5; 0.25 is the Atari standard.",
    )
    parser.add_argument(
        "--no-sticky",
        action="store_true",
        default=False,
        help="Disable StickyActionWrapper entirely (default: enabled).",
    )
    # ---- StompReward arguments ----
    parser.add_argument(
        "--stomp-bonus",
        type=float,
        default=5.0,
        metavar="FLOAT",
        help="Reward per Goomba/Koopa/Beetle stomp (default: 5.0).",
    )
    parser.add_argument(
        "--no-stomp",
        action="store_true",
        default=False,
        help="Disable StompRewardWrapper entirely (default: enabled).",
    )
    # ---- PlatformClimb arguments ----
    parser.add_argument(
        "--climb-bonus",
        type=float,
        default=2.0,
        metavar="FLOAT",
        help="Reward per forward+higher landing (default: 2.0).",
    )
    parser.add_argument(
        "--no-climb",
        action="store_true",
        default=False,
        help="Disable PlatformClimbRewardWrapper entirely (default: enabled).",
    )
    # ---- RND arguments ----
    parser.add_argument(
        "--no-rnd",
        action="store_true",
        default=False,
        help="Disable RND intrinsic motivation. Default: RND enabled.",
    )
    parser.add_argument(
        "--rnd-scale",
        type=float,
        default=1.0,
        metavar="FLOAT",
        help=(
            "RND intrinsic reward multiplier (default: 1.0). "
            "Monitor diagnostics/intrinsic_extrinsic_ratio in TensorBoard:\n"
            "  > 2.0  → reduce --rnd-scale\n"
            "  < 0.1  → increase --rnd-scale\n"
            "  0.3-1.0 → healthy balance"
        ),
    )
    parser.add_argument(
        "--rnd-warmup-steps",
        type=int,
        default=1000,
        metavar="N",
        help="Random VecEnv steps to prime RND obs_rms before training (default: 1000).",
    )
    args = parser.parse_args()

    # ---- Device detection ----
    if torch.cuda.is_available():
        device = "cuda"
    elif torch.backends.mps.is_available():
        device = "mps"
    else:
        device = "cpu"
    print(f"[train] using device: {device}")

    use_rnd = not args.no_rnd

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
        "use_rnd":        use_rnd,
        "rnd_scale":      args.rnd_scale,
    }
    if two_stage:
        curriculum_meta["stage1_steps"]  = args.stage1_steps
        curriculum_meta["stage2_levels"] = stage2_levels
        curriculum_meta["stage2_weights"]= stage2_weights
    with open(os.path.join(args.checkpoint_dir, "curriculum.json"), "w") as f:
        json.dump(curriculum_meta, f, indent=2)

    # ---- Create RND networks (before make_env_fn so they can be closed over) ----
    # Observation shape is obtained from a temporary env WITHOUT RNDWrapper,
    # because RNDWrapper does not change the observation space.
    rnd_target    = None
    rnd_predictor = None
    _obs_shape    = None

    # Print and verify action space size before creating envs.
    _action_check_env = SMBEnv(rom_path=args.rom, lib_path=args.lib)
    _n_actions = _action_check_env.action_space.n
    _action_check_env.close()
    del _action_check_env
    print(f"Action space: {_n_actions} actions")
    assert _n_actions == 14, (
        f"Expected 14 actions, got {_n_actions}. "
        "Check _ACTION_SEQUENCES in smb_env.py."
    )

    if use_rnd:
        _shape_env = SMBEnv(rom_path=args.rom, lib_path=args.lib)
        _obs_shape = (_flat_dim := flat_obs_dim_from_space(_shape_env.observation_space),)
        _shape_env.close()
        del _shape_env

        rnd_target, rnd_predictor = make_rnd_networks(
            obs_shape     = _obs_shape,
            embedding_dim = 512,
            device        = device,
        )
        print(f"[RND] networks created. obs_shape={_obs_shape}, device={device}")
        print(f"[RND] target params:    {sum(p.numel() for p in rnd_target.parameters()):,}")
        print(f"[RND] predictor params: {sum(p.numel() for p in rnd_predictor.parameters()):,}")
        print(f"[RND] intrinsic_scale:  {args.rnd_scale}")

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
    _rnd_label = (
        f"→ RNDWrapper(scale={args.rnd_scale}, device={device})"
        if use_rnd else "(RND disabled)"
    )
    _sticky_label = (
        f"→ StickyActionWrapper(prob={args.sticky_prob})"
        if not args.no_sticky else "(sticky disabled)"
    )
    _stomp_label = (
        f"→ StompRewardWrapper(bonus={args.stomp_bonus})"
        if not args.no_stomp else "(stomp disabled)"
    )
    _climb_label = (
        f"→ PlatformClimbRewardWrapper(bonus={args.climb_bonus})"
        if not args.no_climb else "(climb disabled)"
    )
    print(
        f"Wrapper stack:  SMBEnv"
        f" {_sticky_label}"
        f" → NewMaxXWrapper(scale=2.0, active=False)"
        f" → SurvivalBonusWrapper(bonus=0.02)"
        f" → DeathPenaltyWrapper(penalty=4.0)"
        f" {_stomp_label}"
        f" {_climb_label}"
        f" → VisitedCellsWrapper(cell_bonus={args.cell_bonus}, cell_size=8x8)"
        f" {_rnd_label}"
        f" → SubprocVecEnv → VecMonitor"
    )

    # ---- Headless training envs (start with stage-1 curriculum) ----
    env_fns = [
        make_env_fn(
            args.rom, args.lib,
            levels=stage1_levels, level_weights=stage1_weights,
            rnd_obs_shape=_obs_shape if use_rnd else None,
            rnd_embedding_dim=512,
            rnd_scale=args.rnd_scale,
            cell_bonus=args.cell_bonus,
            sticky_prob=args.sticky_prob,
            stomp_bonus=args.stomp_bonus,
            climb_bonus=args.climb_bonus,
            use_sticky=not args.no_sticky,
            use_stomp=not args.no_stomp,
            use_climb=not args.no_climb,
        )
        for _ in range(args.n_envs)
    ]
    vec_env = SubprocVecEnv(env_fns)

    # ---- Initial RND weight sync ----
    # Workers create fresh random CPU networks in _init().  Sync both target
    # and predictor from the main process so all workers start from the same
    # network state.  This is a one-time operation; subsequent predictor syncs
    # are handled by RNDTrainerCallback after each rollout.
    if use_rnd:
        target_sd_np = {k: v.cpu().numpy() for k, v in rnd_target.state_dict().items()}
        pred_sd_np   = {k: v.cpu().numpy() for k, v in rnd_predictor.state_dict().items()}
        vec_env.env_method("sync_rnd_target",    target_sd_np)
        vec_env.env_method("sync_rnd_predictor", pred_sd_np)
        print("[RND] initial target + predictor weights synced to all workers.")

    vec_env = VecMonitor(vec_env, filename=os.path.join(args.log_dir, "monitor"))

    # ---- RND obs_rms warm-up ----
    # Each RNDWrapper instance (in each subprocess) maintains its own obs_rms.
    # Without warm-up, the first rollout uses obs_rms = (mean=0, var=1), which
    # means the normalised observations may be far from the true distribution.
    # Running random actions primes obs_rms so the first intrinsic rewards are
    # calibrated.  This does NOT affect the start observation for DiagnosticsCallback
    # because that is captured from a separate temporary env below.
    if use_rnd and args.rnd_warmup_steps > 0:
        print(f"[RND] warming up obs_rms via VecEnv ({args.rnd_warmup_steps} random steps) ...")
        _wu_obs = vec_env.reset()
        for _ in range(args.rnd_warmup_steps):
            _wu_actions = np.array([vec_env.action_space.sample()
                                    for _ in range(args.n_envs)])
            _wu_obs, _, _, _ = vec_env.step(_wu_actions)
        print("[RND] obs_rms warm-up complete.")

    # ---- Capture start observation for DiagnosticsCallback Group E ----
    # A temporary single-env instance is used so the live vec_env is NOT touched.
    _tmp_env = make_env_fn(
        args.rom, args.lib,
        levels=stage1_levels, level_weights=stage1_weights,
        # No RND for start_obs capture — not needed and avoids side-effects.
    )()
    _start_obs, _ = _tmp_env.reset()
    _tmp_env.close()
    del _tmp_env

    ppo_kwargs = dict(
        policy          = "MultiInputPolicy",
        env             = vec_env,
        n_steps         = 1024,
        batch_size      = 256,
        n_epochs        = 4,
        learning_rate   = 3e-4,
        gamma           = 0.99,
        gae_lambda      = 0.95,
        clip_range      = 0.2,
        ent_coef        = 0.02,
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

        # ---- VisitedCells warmup on checkpoint resume ----
        # Run n_episodes deterministically on a temporary single env to pre-populate
        # visited_cells with territory the agent has already explored.  Without this,
        # the agent would re-collect huge cell bonuses on familiar ground at the start
        # of resumed training, distorting the value function.
        print("[VisitedCells] warming up visited_cells from resumed checkpoint ...")
        _vcw_env = make_env_fn(
            args.rom, args.lib,
            levels=stage1_levels, level_weights=stage1_weights,
            # No RND for warmup — avoids side-effects on obs_rms in workers.
        )()
        # Walk the wrapper chain to find VisitedCellsWrapper.
        _vcw = _vcw_env
        while not isinstance(_vcw, VisitedCellsWrapper):
            _vcw = _vcw.env
        _vcw.warmup_from_policy(model, n_episodes=20)
        _visited_cells = frozenset(_vcw.visited_cells)   # immutable for IPC safety
        _vcw_env.close()
        del _vcw_env, _vcw
        # Sync the pre-populated archive to all workers.
        vec_env.env_method("set_visited_cells", _visited_cells)
        print(f"[VisitedCells] synced {len(_visited_cells)} cells to all workers.")
    else:
        model = PPO(**ppo_kwargs)

    print(
        "Diagnostics metrics (TensorBoard 'diagnostics/' group):\n"
        "  A — spatial:  max_x_episode, max_x_global\n"
        "  B — death:    steps_before_death, death_penalty_applied, died_before_obstacle\n"
        "  C — survival: survival_bonus_total, survival_fraction\n"
        "  D — entropy:  ent_coef_current, policy_entropy  (every step)\n"
        f"  E — value:    value_at_start_state  (every 10,000 steps, "
        f"start_obs={'captured' if _start_obs else 'MISSING'})\n"
        "  F — RND:      intrinsic_reward_episode_mean, intrinsic_extrinsic_ratio"
        f"  (active: {use_rnd})\n"
        "  G — ground:   frontier_bonus_blocked_rate, max_x_on_ground\n"
        "  H — cells:    cells_visited_episode, cells_visited_total, cells_per_step,\n"
        "                visited_cells_count  (primary exploration progress metric)\n"
        f"  I — combat:   stomps_this_episode  (active: {not args.no_stomp}),\n"
        f"                sticky_action_rate  (active: {not args.no_sticky}),\n"
        f"                climbs_this_episode (active: {not args.no_climb})"
    )
    if use_rnd:
        print(
            "RND TensorBoard metrics (prefix 'rnd/'):\n"
            "  rnd/predictor_loss         — rolling mean of predictor MSE\n"
            "  rnd/intrinsic_reward_mean  — mean intrinsic across envs per step\n"
            "  rnd/intrinsic_reward_max   — max intrinsic per step (spikes = novel state)"
        )

    # ---- Callbacks ----
    # RNDTrainerCallback MUST be first so it trains the predictor and syncs weights
    # to workers BEFORE any other callback reads or logs intrinsic reward stats.
    callbacks = []
    if use_rnd:
        callbacks.append(RNDTrainerCallback(
            rnd_target    = rnd_target,
            rnd_predictor = rnd_predictor,
            lr            = 1e-4,
            batch_size    = 256,
            n_epochs      = 4,
            device        = device,
            max_grad_norm = 0.5,
            verbose       = 1,
        ))

    callbacks += [
        BestRunRecorderCallback(
            rom_path  = args.rom,
            level     = stage1_levels[0],
            video_dir = args.best_run_dir,
            verbose   = 1,
        ),
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
        EntropySchedulerCallback(),
        DiagnosticsCallback(start_obs=_start_obs),
    ]

    if two_stage:
        callbacks.append(CurriculumStageCallback(
            stage2_at      = args.stage1_steps,
            stage2_levels  = stage2_levels,
            stage2_weights = stage2_weights,
            verbose        = 1,
        ))

    # ---- Startup health check (runs before model.learn()) ----
    _cur_ent_coef = model.ent_coef
    if callable(_cur_ent_coef):
        _cur_ent_coef = _cur_ent_coef(1.0)
    try:
        _cur_max_x = vec_env.get_attr("max_x_seen")[0]
    except Exception:
        _cur_max_x = "unavailable"
    _cb_names = [type(cb).__name__ for cb in callbacks]
    print("=" * 60)
    print("TRAINING HEALTH CHECK")
    _stack = (
        "SMBEnv"
        + (" → StickyActionWrapper" if not args.no_sticky else "")
        + " → NewMaxXWrapper(active=False) → SurvivalBonusWrapper"
        " → DeathPenaltyWrapper"
        + (" → StompRewardWrapper" if not args.no_stomp else "")
        + (" → PlatformClimbRewardWrapper" if not args.no_climb else "")
        + " → VisitedCellsWrapper"
        + (" → RNDWrapper" if use_rnd else " (no RNDWrapper)")
        + " → SubprocVecEnv → VecMonitor"
    )
    print(f"Wrapper stack:  {_stack}")
    print(f"Device:         {device}")
    print(f"ent_coef:       {float(_cur_ent_coef)}")
    print(f"max_x_seen:     {_cur_max_x}  (env 0)")
    print(f"Callbacks:      {_cb_names}")
    if use_rnd:
        print(f"RND obs_shape:      {_obs_shape}")
        print(f"RND target params:  {sum(p.numel() for p in rnd_target.parameters()):,}")
        print(f"RND pred params:    {sum(p.numel() for p in rnd_predictor.parameters()):,}")
        print(f"RND intrinsic_scale:{args.rnd_scale}")
        print(f"RND device:         {device}")
    _has_diag    = any(isinstance(cb, DiagnosticsCallback)    for cb in callbacks)
    _has_entropy = any(isinstance(cb, EntropySchedulerCallback) for cb in callbacks)
    _has_rnd     = any(isinstance(cb, RNDTrainerCallback)      for cb in callbacks)
    print(f"DiagnosticsCallback present:      {_has_diag}")
    print(f"EntropySchedulerCallback present: {_has_entropy}")
    print(f"RNDTrainerCallback present:       {_has_rnd}")
    print("Episode end types: "
          "death=obs[game_flags][0] (RAM 0x000E==0x0B) | "
          "complete=info[level_complete] (RAM 0x001D==0x03) | "
          "truncation=stagnation/timeout (no RAM flag)")
    print("=" * 60)

    model.learn(
        total_timesteps     = args.timesteps,
        callback            = callbacks,
        reset_num_timesteps = args.resume is None,
        progress_bar        = True,
    )

    vec_env.close()


if __name__ == "__main__":
    main()
