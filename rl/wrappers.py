"""
Gym wrappers for SMBEnv training.

# ============================================================
# DEATH DETECTION AUDIT (conducted 2026-03-19)
# ============================================================
#
# 1. TERMINATION SIGNAL
#    terminated = dead OR complete  (smb_env.py:343)
#    truncated  = max_steps | stagnation | steps_since_progress (smb_env.py:350-354)
#
# 2. DEATH / COMPLETION KEYS IN `info`
#    NONE. The step() info dict contains only: world_x, frame, stagnating.
#    Neither info["death"] nor info["x_pos"] exist.
#    All wrappers below use obs["game_flags"][0] (dead) and
#    obs["game_flags"][1] (complete) directly from the observation.
#
# 3. DEATH DETECTION MECHANISM
#    player_dead    = (RAM[0x000E] == 0x0B)  — death animation state
#    level_complete = (RAM[0x001D] == 0x03)  — end-of-level game mode
#    Both are read in _get_obs() AFTER all action frames have executed.
#    There is no distinction between death-by-enemy and death-by-pit —
#    both result in state 0x0B. The two are treated identically.
#
# 4. OFF-BY-ONE RISK
#    None found. The step() call sequence is:
#      a) execute all NES frames for the action
#      b) _get_obs()              ← reads RAM after action
#      c) _compute_reward(obs)    ← uses obs["game_flags"] from (b)
#      d) dead = obs["game_flags"][0]  ← same obs
#    Reward, death flag, and x_pos are all consistent and from the same state.
#
# 5. x_pos AT DEATH STEP
#    world_x = _read_world_x() is called after all action frames complete.
#    It is the actual position at the moment of death, not the prior frame.
#
# 6. DEATH vs LEVEL-COMPLETE DISTINCTION
#    Distinguished: obs["game_flags"][0] = dead, obs["game_flags"][1] = complete.
#    They are mutually exclusive in practice (can't die and complete simultaneously).
#    No further sub-typing (enemy kill vs pit fall) is available from the RAM state.
#
# 7. BUGFIX: NewMaxXWrapper — wrong info key
#    Previous implementation read info.get("x_pos", 0.0). The env emits
#    info["world_x"], not info["x_pos"]. This caused max_x_seen to stay at
#    0.0 permanently, making the frontier bonus always 0. Fixed below.
#
# 8. FINDING: base env already applies -10.0 death penalty inside _compute_reward()
#    (smb_env.py:1231). DeathPenaltyWrapper replaces the entire reward on death
#    with -death_penalty to avoid double-penalizing and to cleanly remove the
#    x-progress component that is baked into the reward before this wrapper sees it.
#    Default death_penalty=10.0 matches the base env's value so the magnitude is
#    unchanged; the difference is that x-progress is no longer included.
# ============================================================
"""

import numpy as np
import torch
import torch.nn.functional as F
import gymnasium as gym

from running_mean_std import RunningMeanStd
from rnd import flatten_obs_single, flat_obs_dim_from_space


class NewMaxXWrapper(gym.Wrapper):
    """
    Adds a bonus reward whenever the agent surpasses its lifetime-maximum x position
    while Mario is on the ground.

    This creates a frontier signal: the agent is rewarded for reaching any x it has
    never visited before across all episodes. Because max_x_seen is never reset, the
    bonus is zero once the agent revisits old ground — it only fires at the frontier.
    This breaks the deterministic local optimum where reward gradient vanishes at the
    furthest point reached.

    ON-GROUND GATE (BUGFIX 2026-03-20):
    The frontier bonus and max_x_seen update are gated on Mario being on the ground
    (info["on_ground"], sourced from RAM[0x001D] == 0x00).  Without this gate,
    the bonus fired during void jumps where Mario falls rightward into a pit — the
    agent accumulated frontier reward for airborne x positions that were physically
    unreachable at ground level, incentivizing pit jumps rather than navigable
    progress.  With the gate, only ground-level x progress counts toward the frontier.

    Ground state source: info["on_ground"] (bool, True = on ground, False = airborne).
    RAM[0x001D]: 0x00 = on ground, 0x01 = airborne (jump OR pit fall), 0x03 = level
    complete.  Clears immediately on landing — no multi-frame delay.  Defaults to
    True if the key is absent (safe degradation: treats unknown state as grounded).

    Args:
        env:    The wrapped Gymnasium environment.
        scale:  Multiplier applied to (current_x - max_x_seen) on a new record.
                Default: 2.0.
        active: If False, the wrapper tracks max_x_seen and updates info but does
                NOT add any reward bonus.  Use active=False when VisitedCellsWrapper
                replaces the 1D frontier reward but diagnostic continuity is needed
                (EntropySchedulerCallback still reads max_x_seen via get_attr).
                Default: True.
    """

    def __init__(self, env: gym.Env, scale: float = 2.0, active: bool = True):
        super().__init__(env)
        self.scale = scale
        self.active = active
        self.max_x_seen: float = 0.0  # lifetime max — intentionally never reset

    def reset(self, **kwargs):
        # Do NOT reset max_x_seen here. It is a lifetime maximum across all episodes.
        obs, info = self.env.reset(**kwargs)
        return obs, info

    def step(self, action):
        obs, reward, terminated, truncated, info = self.env.step(action)
        # BUGFIX (original): env emits info["world_x"], not info["x_pos"].
        current_x = float(info.get("world_x", 0.0))
        # BUGFIX (2026-03-20): gate frontier bonus on Mario being on the ground.
        # info["on_ground"] is set by SMBEnv.step() from RAM[0x001D] == 0x00.
        # Default True so the wrapper degrades gracefully if the key is ever absent.
        on_ground = bool(info.get("on_ground", True))

        bonus = 0.0
        if on_ground and current_x > self.max_x_seen:
            if self.active:
                bonus = (current_x - self.max_x_seen) * self.scale
            self.max_x_seen = current_x  # always track, even when inactive

        # Always update info["max_x_seen"] so diagnostics remain accurate regardless
        # of the on_ground gate or active flag.
        info["max_x_seen"]             = self.max_x_seen
        info["mario_on_ground"]        = on_ground
        info["frontier_bonus_blocked"] = (not on_ground) and (current_x > self.max_x_seen)
        return obs, reward + bonus, terminated, truncated, info


class SurvivalBonusWrapper(gym.Wrapper):
    """
    Adds a small reward every timestep Mario is alive.

    Encourages patience and penalizes the "run right and die far away" strategy.
    Without a per-step survival signal, an agent can greedily maximise x-progress
    by sprinting right and dying at the furthest reachable point — the x-reward on
    the death step is still positive. A survival bonus makes staying alive
    intrinsically valuable and creates a gradient against reckless play.

    The bonus is NOT applied on the terminal death step or the level-complete step:
      - Death step: should be penalised, not rewarded for surviving.
      - Level-complete step: already rewarded by the completion bonus in the base env.

    Detection uses obs["game_flags"][0] (dead) and obs["game_flags"][1] (complete),
    which are always present in SMBEnv observations. No info key is used because
    the audit found no death/complete keys in the step() info dict.

    Args:
        env:            The wrapped Gymnasium environment.
        survival_bonus: Reward added per alive, non-terminal step. Default: 0.02.
    """

    def __init__(self, env: gym.Env, survival_bonus: float = 0.02):
        super().__init__(env)
        self.survival_bonus = survival_bonus
        self._total_survival_bonus: float = 0.0

    def reset(self, **kwargs):
        self._total_survival_bonus = 0.0
        obs, info = self.env.reset(**kwargs)
        return obs, info

    def step(self, action):
        obs, reward, terminated, truncated, info = self.env.step(action)
        dead     = obs["game_flags"][0] > 0.5
        complete = obs["game_flags"][1] > 0.5

        if not dead and not complete:
            reward += self.survival_bonus
            self._total_survival_bonus += self.survival_bonus

        info["total_survival_bonus"] = self._total_survival_bonus
        return obs, reward, terminated, truncated, info


class DeathPenaltyWrapper(gym.Wrapper):
    """
    Adds a flat penalty on the death step without touching the x-progress reward.

    The penalty makes death costly, but the x-reward (and all other shaped
    components) is preserved. The new-max-x bonus from NewMaxXWrapper already
    handles the "void-jump" problem more elegantly: it only rewards genuinely
    new ground, so dying far right is only rewarded once per frontier pixel.
    Stripping x-reward on death is therefore unnecessary and was found to
    interact poorly with the survival + frontier signal stack.

    The base env applies its own -10.0 death term inside _compute_reward()
    (smb_env.py:1231). This wrapper's penalty is ADDITIVE with that. With the
    default death_penalty=4.0 the total on-death penalty is roughly -14.0,
    which is bad-but-not-catastrophic relative to the survival bonus scale.

    On level-complete and alive steps the reward is passed through unchanged.
    Detection uses obs["game_flags"][0] (dead) and obs["game_flags"][1] (complete).

    Args:
        env:           The wrapped Gymnasium environment.
        death_penalty: Extra penalty added on the death step (applied as negative).
                       Default: 4.0.
    """

    def __init__(self, env: gym.Env, death_penalty: float = 4.0):
        super().__init__(env)
        self.death_penalty = death_penalty

    def reset(self, **kwargs):
        obs, info = self.env.reset(**kwargs)
        return obs, info

    def step(self, action):
        obs, reward, terminated, truncated, info = self.env.step(action)
        dead     = obs["game_flags"][0] > 0.5
        complete = obs["game_flags"][1] > 0.5

        if dead and not complete:
            reward -= self.death_penalty
            info["death_penalty_applied"] = True
        else:
            info["death_penalty_applied"] = False

        # Expose level_complete so callbacks can distinguish complete from truncation
        # without needing to inspect obs["game_flags"] directly.  No reward impact.
        info["level_complete"] = complete

        return obs, reward, terminated, truncated, info


class RNDWrapper(gym.Wrapper):
    """
    Adds Random Network Distillation (RND) intrinsic rewards on top of
    extrinsic rewards from the underlying environment.

    RND works by training a small *predictor* MLP to match the outputs of
    a frozen *target* MLP on visited observations.  For novel states the
    predictor has high error (high intrinsic reward); for familiar states
    the predictor matches the target well (low intrinsic reward).  This
    pulls the agent toward unexplored territory even when extrinsic reward
    gradients have vanished.

    Non-episodic intrinsic reward (correct RND behaviour):
        The discounted intrinsic return self._ret is NOT reset on episode
        end by default.  This is intentional: curiosity should persist
        across episode boundaries.  Mario dying should not erase the
        novelty signal — the agent still needs to get past that point.
        Set episodic_intrinsic=True only for debugging.

    Observation normalisation:
        A per-instance RunningMeanStd tracks the observation statistics.
        Observations are centred and scaled before being passed to the
        RND networks.  This prevents any single high-magnitude feature
        from dominating the RND signal.

    Subprocess weight synchronisation:
        When used inside SubprocVecEnv, each worker has its own copy of
        the RND networks.  The predictor in each worker starts as a copy
        of the main-process predictor at env-creation time and diverges
        as the main process trains its own predictor.  Call
        sync_rnd_predictor() via env_method() after each predictor
        training round to push updated weights into the workers.

    Info keys added:
        intrinsic_reward  (float) — normalised intrinsic reward this step
        extrinsic_reward  (float) — raw extrinsic reward from inner env
        raw_intrinsic     (float) — unnormalised MSE before scaling

    Args:
        env:               Wrapped Gymnasium environment.
        rnd_target:        Frozen RNDNetwork (requires_grad=False everywhere).
        rnd_predictor:     Trainable RNDNetwork (trained by RNDTrainerCallback).
        intrinsic_scale:   Weight on the normalised intrinsic reward (default 1.0).
        obs_norm_clip:     Clip range for normalised obs (default 5.0).
        reward_norm_gamma: Discount for intrinsic return normaliser (default 0.99).
        episodic_intrinsic: If True, reset the intrinsic return accumulator on
                            episode end (default False = non-episodic).
        device:            torch device string matching the RND networks.
    """

    def __init__(
        self,
        env: gym.Env,
        rnd_target,
        rnd_predictor,
        intrinsic_scale:    float = 1.0,
        obs_norm_clip:      float = 5.0,
        reward_norm_gamma:  float = 0.99,
        episodic_intrinsic: bool  = False,
        device:             str   = "cpu",
    ):
        super().__init__(env)
        self.rnd_target        = rnd_target
        self.rnd_predictor     = rnd_predictor
        self.intrinsic_scale   = intrinsic_scale
        self._obs_norm_clip    = obs_norm_clip
        self._gamma            = reward_norm_gamma
        self._episodic         = episodic_intrinsic
        self._device           = device

        flat_dim = flat_obs_dim_from_space(env.observation_space)
        self.obs_rms    = RunningMeanStd(shape=(flat_dim,))
        self.reward_rms = RunningMeanStd(shape=())
        self._ret: float = 0.0

    # ------------------------------------------------------------------
    def reset(self, *, seed=None, options=None):
        obs, info = self.env.reset(seed=seed, options=options)
        # obs_rms and reward_rms are lifetime statistics — never reset.
        # _ret: non-episodic by default; only reset if explicitly requested.
        if self._episodic:
            self._ret = 0.0
        return obs, info

    def step(self, action):
        obs, extrinsic_reward, terminated, truncated, info = self.env.step(action)

        # Compute intrinsic reward from RND networks.
        raw_intrinsic = self._compute_intrinsic(obs)

        # Normalise intrinsic reward via discounted intrinsic return.
        self._ret = self._ret * self._gamma + raw_intrinsic
        self.reward_rms.update(np.array([self._ret]))
        intrinsic_norm = raw_intrinsic / (np.sqrt(self.reward_rms.var[()]) + 1e-8)

        # Non-episodic: only reset on done if episodic mode is explicitly on.
        if self._episodic and (terminated or truncated):
            self._ret = 0.0

        total_reward = float(extrinsic_reward) + self.intrinsic_scale * float(intrinsic_norm)

        info["intrinsic_reward"] = float(intrinsic_norm)
        info["extrinsic_reward"] = float(extrinsic_reward)
        info["raw_intrinsic"]    = float(raw_intrinsic)

        return obs, total_reward, terminated, truncated, info

    # ------------------------------------------------------------------
    def _compute_intrinsic(self, obs: dict) -> float:
        """
        Compute raw intrinsic reward for one observation.

        The obs dict is flattened to a 1-D vector, normalised via obs_rms,
        and passed through the frozen target and trainable predictor
        networks.  The MSE between their outputs is the intrinsic reward.

        No gradients are computed here — the predictor is trained
        separately in RNDTrainerCallback._on_rollout_end.
        """
        flat = flatten_obs_single(obs).astype(np.float32)

        # Update running observation statistics.
        self.obs_rms.update(flat[np.newaxis])   # shape (1, flat_dim)

        # Normalise and clip.
        obs_norm = np.clip(
            (flat - self.obs_rms.mean.astype(np.float32)) / self.obs_rms.std.astype(np.float32),
            -self._obs_norm_clip,
            self._obs_norm_clip,
        ).astype(np.float32)

        obs_t = torch.from_numpy(obs_norm).unsqueeze(0).to(self._device)   # (1, flat_dim)

        with torch.no_grad():
            feat_target = self.rnd_target(obs_t)
            feat_pred   = self.rnd_predictor(obs_t)

        return float(F.mse_loss(feat_pred, feat_target).item())

    # ------------------------------------------------------------------
    def sync_rnd_predictor(self, state_dict_numpy: dict) -> None:
        """
        Load predictor weights pushed from the main process.

        Called via SubprocVecEnv.env_method("sync_rnd_predictor", weights)
        after RNDTrainerCallback trains the main-process predictor.
        This keeps the subprocess predictors current so intrinsic rewards
        reflect genuine novelty rather than the initial random state.

        Args:
            state_dict_numpy: Dict mapping parameter name → numpy array
                              (serialised from the main-process predictor's
                              state_dict via .cpu().numpy()).
        """
        state_dict = {
            k: torch.from_numpy(v).to(self._device)
            for k, v in state_dict_numpy.items()
        }
        self.rnd_predictor.load_state_dict(state_dict)

    def sync_rnd_target(self, state_dict_numpy: dict) -> None:
        """
        Load target weights pushed from the main process (called once at startup).

        Workers create their own randomly-initialized target; this sync ensures
        all workers use the same frozen target as the main process. Without it,
        the main-process predictor (trained against the main target) would produce
        wrong intrinsic rewards when synced to workers with different targets.

        Args:
            state_dict_numpy: Dict mapping parameter name → numpy array
                              (serialised from the main-process target's
                              state_dict via .cpu().numpy()).
        """
        state_dict = {
            k: torch.from_numpy(v).to(self._device)
            for k, v in state_dict_numpy.items()
        }
        self.rnd_target.load_state_dict(state_dict)
        self.rnd_target.eval()  # target must stay in eval mode


class VisitedCellsWrapper(gym.Wrapper):
    """
    Rewards Mario for visiting new (world_x, screen_y) tile cells while on the ground.

    WHY 2D CELLS INSTEAD OF 1D X-FRONTIER:
    NewMaxXWrapper's 1D frontier only rewards rightward progress: the agent gets bonus
    for any x position never seen before, but two positions at the same x on different
    height platforms are treated identically. This causes a failure mode where the agent
    finds a lower platform edge, collects the x-frontier bonus up to that point, then
    stalls — there is no gradient pointing upward to a higher platform at the same x
    that leads forward. The 2D cell bonus rewards any new (cx, cy) ground-level
    position, so higher platforms at the same x are distinct cells that carry reward.

    THE LOCAL OPTIMUM THIS SOLVES:
    The agent plateaued at x≈650 on a lower platform edge. The correct path requires
    jumping up to a higher platform at roughly the same x. With a 1D x-only frontier,
    both the lower and higher platforms map to the same frontier value — there is no
    incentive to choose the higher one. With 2D cells, the higher platform is a
    distinct (cx, cy_high) cell that has never been visited, carrying a fresh
    cell_bonus. This breaks the symmetry and pulls the agent upward.

    CELL LIFETIME PERSISTENCE:
    visited_cells is NEVER reset between episodes. Like max_x_seen in NewMaxXWrapper,
    this is intentional: once a cell has been visited, it offers no further exploration
    reward. This creates a curriculum — early training collects dense rewards on
    familiar territory, and as the frontier advances the reward gradient naturally
    concentrates at the edge of known space. Resetting on episode end would re-reward
    familiar territory every episode, destroying the curriculum pressure.

    CELL SIZE TUNING:
    cell_size_x = 16: one NES tile width in world_x pixels — matches the game's
        native tile grid. Each horizontal tile is one cell.
    cell_size_y = 16: one NES tile height in screen_y pixels — each platform layer
        is one cell vertically.
    Too large (e.g. 64): many positions map to the same cell, sparse reward signal.
    Too small (e.g. 1): every pixel is a new cell, reward floods in the first episode.

    # Cell size tuning: if cells_visited_total grows too fast (agent collects all
    # cells in first 100k steps), increase cell_size. If cells_visited_total is flat
    # after 200k steps, decrease cell_size or increase cell_bonus.
    # Monitor diagnostics/cells_per_step — healthy range is 0.01-0.05
    # (1-5 new cells per 100 steps during active exploration).

    ON-GROUND GATE:
    New cells are only registered when info["on_ground"] is True (RAM[0x001D] == 0x00).
    Airborne x/y positions (during jumps or pit falls) do not count. Only physically
    navigable ground-level positions advance the exploration frontier.

    Info keys added each step:
        new_cell_found       (bool)  — True if this step registered a new cell
        total_cells_visited  (int)   — lifetime total cells across all episodes
        episode_new_cells    (int)   — new cells found so far this episode (reset each reset())

    Args:
        env:          The wrapped Gymnasium environment.
        cell_size_x:  World-x units per cell (default 16 = one NES tile width).
        cell_size_y:  Screen-y units per cell (default 16 = one NES tile height).
        cell_bonus:   Reward added per newly discovered cell (default 1.0).
    """

    def __init__(
        self,
        env:         gym.Env,
        cell_size_x: int   = 16,
        cell_size_y: int   = 16,
        cell_bonus:  float = 1.0,
    ):
        super().__init__(env)
        self.cell_size_x = cell_size_x
        self.cell_size_y = cell_size_y
        self.cell_bonus  = cell_bonus

        self.visited_cells: set      = set()   # lifetime — never reset
        self._episode_new_cells: int = 0       # reset each episode
        self._total_cells:       int = 0       # lifetime total (== len(visited_cells))

    def reset(self, **kwargs):
        self._episode_new_cells = 0
        # Do NOT reset visited_cells or _total_cells — lifetime persistence.
        obs, info = self.env.reset(**kwargs)
        return obs, info

    def step(self, action):
        obs, reward, terminated, truncated, info = self.env.step(action)

        current_x = float(info.get("world_x", 0.0))
        current_y = float(info.get("mario_y",  0.0))
        on_ground = bool(info.get("on_ground", True))

        cell = (int(current_x // self.cell_size_x),
                int(current_y // self.cell_size_y))

        new_cell = False
        if on_ground and cell not in self.visited_cells:
            self.visited_cells.add(cell)
            reward += self.cell_bonus
            self._episode_new_cells += 1
            self._total_cells += 1
            new_cell = True

        info["new_cell_found"]      = new_cell
        info["total_cells_visited"] = self._total_cells
        info["episode_new_cells"]   = self._episode_new_cells
        return obs, reward, terminated, truncated, info

    def set_visited_cells(self, cells: set) -> None:
        """
        Replace visited_cells with the provided set and resync _total_cells.

        Called via SubprocVecEnv.env_method("set_visited_cells", cells) after
        loading a checkpoint, to pre-populate the archive with territory the
        agent has already explored so resumed training starts from the true
        frontier rather than re-rewarding familiar ground.
        """
        self.visited_cells = set(cells)
        self._total_cells  = len(self.visited_cells)

    def warmup_from_policy(self, model, n_episodes: int = 10) -> None:
        """
        Run n_episodes deterministically to pre-populate visited_cells.

        Call this on a single-env instance after loading a checkpoint, before
        creating SubprocVecEnv.  The resulting visited_cells set is then synced
        to all workers via env_method("set_visited_cells", wrapper.visited_cells).

        Args:
            model:      A loaded SB3 model with a predict() method.
            n_episodes: Number of episodes to run for warm-up (default 10).
        """
        obs, _ = self.reset()
        episodes_done = 0
        while episodes_done < n_episodes:
            action, _ = model.predict(obs, deterministic=True)
            obs, _, terminated, truncated, _ = self.step(action)
            if terminated or truncated:
                obs, _ = self.reset()
                episodes_done += 1
        print(
            f"[VisitedCells] warmup complete: {self._total_cells} cells "
            f"from {n_episodes} episodes."
        )


# ---------------------------------------------------------------------------
# Stompable enemy types (from RAM[0x000F+i])
# ---------------------------------------------------------------------------
# Goomba=0x01, GreenKoopa=0x02, RedKoopa=0x03, BuzzyBeetle=0x06
# NOT included: PiranhaPlant=0x09, HammerBros=0x0A, Bowser=0x0B
_STOMPABLE_TYPES: frozenset = frozenset({0x01, 0x02, 0x03, 0x06})


class StickyActionWrapper(gym.Wrapper):
    """
    With probability sticky_prob, repeat the previous action instead of executing
    the new one from the policy.

    This encourages sustained button-holds across env steps — most notably, holding
    A for full jump arcs.  The base env's action space uses multi-frame motor
    primitives where a single FULL_JUMP_R already holds A for 9 NES frames, but
    sticky actions effectively chain multiple JUMP steps together, allowing the
    agent to execute repeated jumps or sustained A-button input without requiring
    the policy to explicitly output the same action multiple times in a row.

    The wrapper is transparent to observation and reward — it only modifies which
    action is passed down to the base env.

    Info keys added:
        action_was_sticky (bool) — True if the previous action was repeated this step.

    Args:
        env:         The wrapped Gymnasium environment.
        sticky_prob: Probability of repeating the previous action (default 0.25).
    """

    def __init__(self, env: gym.Env, sticky_prob: float = 0.25):
        super().__init__(env)
        self.sticky_prob  = sticky_prob
        self._last_action: int = 0
        self._rng = np.random.default_rng()

    def reset(self, **kwargs):
        self._last_action = 0
        obs, info = self.env.reset(**kwargs)
        return obs, info

    def step(self, action):
        was_sticky = False
        if self._rng.random() < self.sticky_prob:
            action     = self._last_action
            was_sticky = True
        self._last_action = int(action)
        obs, reward, terminated, truncated, info = self.env.step(action)
        info["action_was_sticky"] = was_sticky
        return obs, reward, terminated, truncated, info


class StompRewardWrapper(gym.Wrapper):
    """
    Rewards Mario for stomping stompable enemies.

    Stompable types: Goomba (0x01), Green Koopa (0x02), Red Koopa (0x03),
    Buzzy Beetle (0x06).  Piranha Plants, Hammer Bros, and Bowser are excluded.

    DETECTION MECHANISM:
    A stomp is detected when all of the following hold on consecutive steps:
      1. Enemy slot i was alive with a stompable type on the PREVIOUS step.
      2. Enemy slot i is gone (alive flag = 0) on the CURRENT step.
      3. Mario was FALLING on the PREVIOUS step (prev_vy > 0.05, positive = down).
      4. Enemy was in plausible stomp range on the PREVIOUS step:
         prev_rel_y in [-0.2, 0.5] — enemy within ~24-60 px below Mario, or
         slightly above (Mario's feet crossing the enemy's top).

    Checking PREVIOUS step state avoids the off-by-one where the enemy has already
    vanished by the time we'd read it in the current obs.

    Enemy data source: obs["enemies"] (5,8) float32 from _build_enemies():
        index 0: etype/15.0  → round(val*15) to recover integer type
        index 2: (ey-mario_sy)/120.0  → rel_y  (positive = enemy below Mario)
        index 4: vy_raw/80.0 (populated by frame-delta in _get_obs)
        index 7: 1.0 if alive, 0.0 if dead
    Mario vy: obs["player_state"][4] = vy_raw/80.0, positive = falling (NES y-down).

    DETECTION FAILURE HANDLING:
    When an enemy disappears and Mario WAS falling but the position check fails,
    `_stomp_detection_failures` increments.  After 100 such failures a warning
    is issued and the wrapper disables itself.  This catches systematic signal
    failures (broken vy or rel_y) without silently producing zero stomp rewards.
    Non-stomp enemy deaths (fireball, Starman, pit fall) when Mario is NOT falling
    are NOT counted as failures — they are normal gameplay.

    Info keys added each step:
        stomps_this_episode    (int)  — stomps rewarded so far this episode
        stomp_detection_active (bool) — False if wrapper has self-disabled

    Args:
        env:         The wrapped Gymnasium environment.
        stomp_bonus: Reward per confirmed stomp (default 5.0).
    """

    def __init__(self, env: gym.Env, stomp_bonus: float = 5.0):
        super().__init__(env)
        self.stomp_bonus = stomp_bonus

        self._disabled:                  bool          = False
        self._stomp_detection_failures:  int           = 0
        self._stomps_this_episode:       int           = 0

        # Previous-step cache (sized for 5 enemy slots)
        self._prev_etypes: np.ndarray = np.zeros(5, dtype=np.int32)
        self._prev_alive:  np.ndarray = np.zeros(5, dtype=bool)
        self._prev_rel_y:  np.ndarray = np.zeros(5, dtype=np.float32)
        self._prev_vy:     float      = 0.0   # Mario vy last step, positive=falling

    def _update_prev_state(self, obs: dict) -> None:
        enemies = obs["enemies"]   # (5, 8)
        for i in range(5):
            self._prev_etypes[i] = round(float(enemies[i, 0]) * 15.0)
            self._prev_alive[i]  = bool(enemies[i, 7] > 0.5)
            self._prev_rel_y[i]  = float(enemies[i, 2])
        self._prev_vy = float(obs["player_state"][4])   # vy_raw/80.0, positive=falling

    def reset(self, **kwargs):
        self._stomps_this_episode = 0
        obs, info = self.env.reset(**kwargs)
        self._update_prev_state(obs)
        return obs, info

    def step(self, action):
        obs, reward, terminated, truncated, info = self.env.step(action)

        stomp_bonus_total = 0.0
        if not self._disabled:
            enemies       = obs["enemies"]   # (5, 8)
            mario_falling = self._prev_vy > 0.05   # was Mario falling last step?

            for i in range(5):
                if not self._prev_alive[i]:
                    continue   # was already dead last step
                if self._prev_etypes[i] not in _STOMPABLE_TYPES:
                    continue   # not a stompable enemy

                if bool(enemies[i, 7] > 0.5):
                    continue   # enemy still alive this step

                # Enemy was alive with stompable type, now gone.
                position_ok = -0.2 <= self._prev_rel_y[i] <= 0.5
                if mario_falling and position_ok:
                    stomp_bonus_total        += self.stomp_bonus
                    self._stomps_this_episode += 1
                elif mario_falling and not position_ok:
                    # Looks like a stomp attempt (falling + enemy gone) but position
                    # check failed — may indicate broken rel_y or vy signal.
                    self._stomp_detection_failures += 1
                    if self._stomp_detection_failures > 100:
                        import warnings
                        warnings.warn(
                            f"[StompRewardWrapper] {self._stomp_detection_failures} "
                            "stomp detection failures (mario falling + enemy gone but "
                            "position check failed). Disabling stomp rewards. "
                            "Check prev_rel_y range [-0.2, 0.5] or vy signal.",
                            stacklevel=2,
                        )
                        self._disabled = True
                        break

        reward += stomp_bonus_total
        self._update_prev_state(obs)

        info["stomps_this_episode"]    = self._stomps_this_episode
        info["stomp_detection_active"] = not self._disabled
        return obs, reward, terminated, truncated, info
