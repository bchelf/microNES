<!-- Last audited: 2026-03-20 -->
<!-- RND added: 2026-03-19 -->
<!-- on_ground fix: 2026-03-20 (RAM[0x001D]==0x00 confirmed via frame-level diff) -->
<!-- VisitedCellsWrapper added: 2026-03-20 (replaces NewMaxXWrapper 1D frontier reward) -->
<!-- StickyActionWrapper + StompRewardWrapper + PlatformClimbRewardWrapper added: 2026-03-20 -->
<!-- Audit confidence: INFO KEYS=HIGH | WRAPPER STACK=HIGH | REWARD=HIGH | CALLBACKS=HIGH | BUGS=HIGH | METRICS=HIGH | RND=HIGH -->

# CLAUDE.md — SMB RL Training Project

## Project Overview

PPO agent (Stable-Baselines3) trained on Super Mario Bros 1 via a custom Gymnasium environment (`SMBEnv`) that wraps a purpose-built NES emulator shared library (`libmicrones_rl`). Observations are structured RAM and PPU nametable features — no pixels. Random Network Distillation (RND) provides intrinsic curiosity rewards to pull the agent past hard obstacles. The environment is part of a larger project targeting RP2350 hardware; the RL stack lives entirely in `rl/`.

---

## Critical Facts — Read Before Touching Anything

### Info Key Reference

These are the **only** keys present in `info` after a `step()` call. Any other key you expect (e.g. `info["death"]`, `info["x_pos"]`) does not exist.

| Key | Type | Set by | Description | Notes |
|-----|------|--------|-------------|-------|
| `world_x` | `int` | `SMBEnv.step()` | Absolute x position in level. `RAM[0x006D]*256 + RAM[0x0086]` | The canonical x position. Use this everywhere. |
| `frame` | `int` | `SMBEnv.step()` | NES frame count since last reset | |
| `stagnating` | `bool` | `SMBEnv.step()` | True when max x progress < 64px over last 100 steps | |
| `max_x_seen` | `float` | `NewMaxXWrapper.step()` | **Lifetime** max x across all episodes — never reset | Not per-episode. See wrapper note. |
| `total_survival_bonus` | `float` | `SurvivalBonusWrapper.step()` | Cumulative survival bonus for current episode | Reset to 0.0 on episode reset |
| `death_penalty_applied` | `bool` | `DeathPenaltyWrapper.step()` | True only on the death step | Reliable termination indicator |
| `level_complete` | `bool` | `DeathPenaltyWrapper.step()` | True only on the level-complete step | Added so callbacks don't need to inspect obs directly |
| `intrinsic_reward` | `float` | `RNDWrapper.step()` | Normalised intrinsic reward this step | Only present when RND enabled. Used by DiagnosticsCallback Group F. |
| `extrinsic_reward` | `float` | `RNDWrapper.step()` | Raw extrinsic reward from inner env (before intrinsic addition) | Only present when RND enabled. |
| `raw_intrinsic` | `float` | `RNDWrapper.step()` | Un-normalised MSE between predictor and target | Only present when RND enabled. For debugging scale. |
| `on_ground` | `bool` | `SMBEnv.step()` | True when Mario is on the ground. Source: `RAM[0x001D] == 0x00`. 0x01 = airborne (jump or pit fall), 0x03 = level complete. Clears immediately on landing. | Foundation for on-ground gating in NewMaxXWrapper and VisitedCellsWrapper. |
| `mario_y` | `int` | `SMBEnv.step()` | Screen-Y pixel position (`RAM[0x00CE]`). 0 = top, ~240 = bottom. Ground level ≈ 176. Higher platforms have lower values (e.g. 128, 144). | Used by VisitedCellsWrapper for 2D cell y-coordinate. |
| `mario_on_ground` | `bool` | `NewMaxXWrapper.step()` | True when Mario is on the ground. Reads `info["on_ground"]` from SMBEnv. | Used by DiagnosticsCallback for Group G metrics. |
| `frontier_bonus_blocked` | `bool` | `NewMaxXWrapper.step()` | True when Mario was airborne and would have set a new max_x but the frontier bonus was gated. | Key verification metric for the on-ground bug fix (2026-03-20). |
| `action_was_sticky` | `bool` | `StickyActionWrapper.step()` | True when the previous action was repeated instead of the policy's action. | Only present when StickyActionWrapper is in the stack (absent with `--no-sticky`). |
| `stomps_this_episode` | `int` | `StompRewardWrapper.step()` | Cumulative stomps rewarded so far this episode. Reset to 0 on reset(). | Only present when StompRewardWrapper is in the stack (absent with `--no-stomp`). |
| `stomp_detection_active` | `bool` | `StompRewardWrapper.step()` | False if StompRewardWrapper self-disabled after >100 detection failures. | Monitor if `stomps_this_episode` is unexpectedly always 0. |
| `climbs_this_episode` | `int` | `PlatformClimbRewardWrapper.step()` | Cumulative forward+higher landings rewarded this episode. Reset to 0 on reset(). | Only present when PlatformClimbRewardWrapper is in the stack (absent with `--no-climb`). |
| `episode` | `dict` | `VecMonitor` | Contains `r`, `l`, `t` at episode end only | Only present in the final step info of an episode. Note: `r` includes intrinsic reward (it is part of the total env reward). |

**CRITICAL: There is no `info["death"]` key and no `info["x_pos"]` key.** Death is read from `obs["game_flags"][0]`. Level complete is read from `obs["game_flags"][1]` or `info["level_complete"]`.

---

### Episode Termination Types

**Death** (`terminated=True, truncated=False`):
- RAM address: `RAM[0x000E] == 0x0B` (death animation state)
- Detected in `_get_obs()` → `obs["game_flags"][0] = 1.0`
- In `step()`: `terminated = dead or complete` (line 343)
- No distinction between death-by-enemy and death-by-pit — both produce state `0x0B`
- `info["death_penalty_applied"]` will be `True`
- `info["level_complete"]` will be `False`

**Level Complete** (`terminated=True, truncated=False`):
- RAM address: `RAM[0x001D] == 0x03` (end-of-level game mode)
- Detected in `_get_obs()` → `obs["game_flags"][1] = 1.0`
- `info["level_complete"]` will be `True`
- `info["death_penalty_applied"]` will be `False`

**Truncation** (`terminated=False, truncated=True`):
Three causes, all in `SMBEnv.step()` lines 345–354:
1. `step_count >= MAX_STEPS` (MAX_STEPS = 20,000)
2. Stagnation: max x progress < 64px over last 100 steps
3. Steps since last x-progress ≥ `STAGNATION_EARLY_STOP` (300 steps)

Truncation does **not** set `info["death_penalty_applied"]` or `info["level_complete"]`. `DeathPenaltyWrapper` does not apply its penalty on truncation — verified in code.

**Timing note:** `_get_obs()` is called after all NES action frames complete. `_compute_reward()` uses the same `obs`. Reward, death flag, and `world_x` are all consistent and from the same post-action RAM state. No off-by-one.

---

### Wrapper Stack (innermost → outermost)

Applied in `make_env_fn()` in `train_smb_rl.py`:

```
SMBEnv
  → StickyActionWrapper(sticky_prob=0.25)          --no-sticky to disable
    → NewMaxXWrapper(scale=2.0, active=False)        diagnostic only — no reward
      → SurvivalBonusWrapper(survival_bonus=0.02)
        → DeathPenaltyWrapper(death_penalty=4.0)
          → StompRewardWrapper(stomp_bonus=5.0)      --no-stomp to disable
            → PlatformClimbRewardWrapper(climb_bonus=2.0)  --no-climb to disable
              → VisitedCellsWrapper(cell_bonus=1.0, cell_size=8×8)
                → RNDWrapper(intrinsic_scale=1.0)    --no-rnd to disable
                  → SubprocVecEnv (8 workers)
                    → VecMonitor
```

| Layer | Reward modification | Info keys added |
|-------|--------------------|--------------------|
| `SMBEnv` | Full reward from `_compute_reward()` — see Reward Structure | `world_x`, `frame`, `stagnating`, `on_ground`, `mario_y` |
| `StickyActionWrapper` | **None** — action-only modification | `action_was_sticky` |
| `NewMaxXWrapper` | **None** (active=False — diagnostic only) | `max_x_seen`, `mario_on_ground`, `frontier_bonus_blocked` |
| `SurvivalBonusWrapper` | +`0.02` per alive non-terminal step | `total_survival_bonus` |
| `DeathPenaltyWrapper` | `-4.0` additive on death step | `death_penalty_applied`, `level_complete` |
| `StompRewardWrapper` | +`stomp_bonus` (+5.0) per stomp of Goomba/GreenKoopa/RedKoopa/BuzzyBeetle | `stomps_this_episode`, `stomp_detection_active` |
| `PlatformClimbRewardWrapper` | +`climb_bonus` (+2.0) per landing that is forward AND higher than takeoff | `climbs_this_episode` |
| `VisitedCellsWrapper` | +`cell_bonus` when on_ground AND new (cx,cy) cell | `new_cell_found`, `total_cells_visited`, `episode_new_cells` |
| `RNDWrapper` | +`intrinsic_scale * intrinsic_norm` every step | `intrinsic_reward`, `extrinsic_reward`, `raw_intrinsic` |
| `SubprocVecEnv` | None | None |
| `VecMonitor` | None | `episode` dict at episode end |

`NewMaxXWrapper.max_x_seen` is **never reset** between episodes. It is a lifetime maximum. `reset()` explicitly does not touch it. This is intentional — it creates a frontier signal that degrades to zero once familiar ground is revisited.

`get_attr("max_x_seen")` from the VecEnv works because `gym.Wrapper.__getattr__` delegates unknown attributes down the chain through `DeathPenalty → Survival → NewMaxXWrapper`.

---

## Reward Structure

All components listed in order of application. The base env clips total to `[-15.0, 15.0]` before wrappers see it.

| Component | Source | Scale | Trigger | Notes |
|-----------|--------|-------|---------|-------|
| Route viability potential | `SMBEnv._compute_reward()` | `0.5 * delta_V` | Every step | Ng-style shaping. Zero-sum over episode. `delta_V` can be negative (doom entry) |
| Dense forward progress | `SMBEnv._compute_reward()` | `clip(dx/40, 0, 1.5) * viability_factor` | dx > 0 | `viability_factor = 0.2 + 0.8 * V_now` |
| Backtrack penalty | `SMBEnv._compute_reward()` | `clip(dx/40, -0.5, 0)` | dx < 0 | |
| Alive bonus | `SMBEnv._compute_reward()` | `+0.002` | Every step | **Separate from SurvivalBonusWrapper's bonus** |
| Death penalty (base) | `SMBEnv._compute_reward()` | `-10.0` | Death step | |
| Level completion | `SMBEnv._compute_reward()` | `+10.0 + time_bonus` | Complete step | `time_bonus` = up to `+5.0` |
| Unnecessary jump penalty | `SMBEnv._compute_reward()` | `-0.005` | Jump with no obstacle/gap ahead | |
| Landing bonus | `SMBEnv._compute_reward()` | `+0.2` | Landing on viability-improving surface | `delta_V > 0.10` required |
| Doomed-state penalty | `SMBEnv._compute_reward()` | `-0.3` | Viability drops below 0.15 with delta < -0.25 | One-shot on entry |
| New-episode-max-x | `SMBEnv._compute_reward()` | `+0.05 * new_delta_x` | New episode-max x | Episode max resets each episode |
| Stagnation penalty | `SMBEnv._compute_reward()` | `-0.01` per step | After 120 steps without new max x | |
| **Base env reward clip** | `SMBEnv._compute_reward()` | `[-15.0, 15.0]` | Always | Applied before wrappers |
| Frontier bonus (lifetime) | `NewMaxXWrapper` | **disabled** (`active=False`) | — | `max_x_seen` still tracked for diagnostics; no reward |
| Stomp bonus | `StompRewardWrapper` | `+stomp_bonus` (default `+5.0`) | Stomping Goomba/GreenKoopa/RedKoopa/BuzzyBeetle | Detected via: enemy alive→dead + mario_falling + position in range. Disabled with `--no-stomp`. |
| Platform climb bonus | `PlatformClimbRewardWrapper` | `+climb_bonus` (default `+2.0`) | Landing that is both forward (world_x >) AND higher (mario_y <) than takeoff position | One bonus per landing event (False→True on_ground transition). Disabled with `--no-climb`. |
| Cell exploration bonus | `VisitedCellsWrapper` | `+cell_bonus` (default `+1.0`) | New ground-level `(cx,cy)` tile cell | Replaces the 1D x-frontier bonus with a 2D signal |
| Survival bonus | `SurvivalBonusWrapper` | `+0.02` per step | Alive non-terminal step | **See double-counting note** |
| Extra death penalty | `DeathPenaltyWrapper` | `-4.0` | Death step | Additive on top of base env's `-10.0` |

**Double-counting notes:**

1. **Two alive bonuses:** The base env adds `+0.002` per step AND `SurvivalBonusWrapper` adds `+0.02` per step. Combined alive-step bonus = `+0.022`. Not a bug per se, but the base env's `+0.002` is often forgotten.

2. **Two x-progress bonuses:** The base env has a new-episode-max-x bonus (`0.05 * delta`) that resets each episode, AND `NewMaxXWrapper` has a lifetime-max-x bonus (`2.0 * delta`). They fire at the same time when progress is also a new lifetime record. This is intentional — the episode bonus provides denser signal; the lifetime bonus creates long-range curriculum pressure.

3. **Total on-death penalty: approximately `-14.0`** (base `-10.0` + wrapper `-4.0`) before considering clipping and other components on that step. The `[-15.0, 15.0]` clip is applied inside `_compute_reward()`, so the wrapper's `-4.0` is applied *after* clipping.

---

## Observation Space

`MultiInputPolicy` receives a `Dict` observation with 20 keys:

| Key | Shape | Range | Description |
|-----|-------|-------|-------------|
| `player_state` | (12,) | [-1,1] | world_x, screen_x/y, vx, vy, on_ground, facing_right, crouching, anim, power, invincibility, in_transition |
| `level_context` | (6,) | [0,1] | world_num, level_num, area_offset, scroll_px, time_remaining, scroll_page_hi |
| `game_flags` | (4,) | [0,1] | **[dead, level_complete, in_pipe, in_transition]** — index 0 and 1 are the primary termination signals |
| `player_speed` | (1,) | [0,1] | |
| `jump_phase` | (1,) | [0,1] | 0=ground, 1/3=rising, 2/3=apex, 1=falling (normalized) |
| `distance_to_ground` | (1,) | [0,1] | |
| `gap_ahead` | (5,) | [0,1] | 1.0 if gap in that lookahead column |
| `obstacle_ahead` | (5,) | [0,1] | 1.0 if solid tile at body height in that column |
| `enemy_threat` | (5,) | [0,1] | 1.0 if enemy in that lookahead column |
| `dist_to_next_enemy` | (1,) | [0,1] | |
| `dist_to_next_gap` | (1,) | [0,1] | |
| `dist_to_next_pipe` | (1,) | [0,1] | |
| `action_history` | (4,) | [0,1] | Last 4 actions, normalized |
| `tile_grid` | (13,17) | [0,1] | Semantic tile categories (8 types) |
| `enemies` | (5,8) | [-1,1] | 5 enemy slots × 8 features (pos, vel, type, etc.) |
| `objects` | (10,) | [-1,1] | Powerup (4 features) + 2 fireballs (3 features each) |
| `dynamic_context` | (10,) | [-1,1] | Timing + nearest enemy dynamics + collision heuristics |
| `platform_topology` | (18,) | [0,1] | Reachable landings, runway, route branching ahead |
| `trajectory_memory` | (14,) | [-1,1] | Recent motion, time-since events, support state |
| `route_viability` | (11,) | [0,1] | Viability score + dead-end signals + doomed flag |

Action space: `Discrete(14)` — 14 motor primitives (WAIT, STEP_RIGHT, RUN_RIGHT, STEP_LEFT, RUN_LEFT, SHORT_JUMP_R, FULL_JUMP_R, SHORT_JUMP_L, FULL_JUMP_L, SHORT_JUMP_IP, FULL_JUMP_IP, BRAKE, CROUCH, FIREBALL).

---

## Active Metrics in TensorBoard

### `rollout/` (SB3 built-in)
| Metric | Description | Reliability |
|--------|-------------|-------------|
| `rollout/ep_rew_mean` | Mean episode reward over rollout | **Unreliable as plateau signal.** Dominated by the `-10.0` / `-14.0` death penalty; small policy improvements produce negligible change relative to the death-penalty variance. `EntropySchedulerCallback` uses this anyway — a known limitation. |
| `rollout/ep_len_mean` | Mean episode length | Reliable |

### `train/` (SB3 built-in + EntropyScheduler)
| Metric | Description | Reliability |
|--------|-------------|-------------|
| `train/entropy_loss` | SB3 entropy loss (negative entropy) | Reliable |
| `train/policy_gradient_loss` | PPO policy loss | Reliable |
| `train/value_loss` | Value function loss | Reliable |
| `train/approx_kl` | Approximate KL divergence | Reliable |
| `train/clip_fraction` | PPO clip fraction | Reliable |
| `train/explained_variance` | Value function explained variance | Reliable |
| `train/ent_coef_scheduled` | ent_coef as managed by `EntropySchedulerCallback` | Reliable — logged every step |

### `rnd/` (RNDTrainerCallback)
| Metric | Description | Reliability |
|--------|-------------|-------------|
| `rnd/predictor_loss` | Rolling mean MSE between predictor and frozen target over last 100 minibatches. Starts high, decreases as predictor learns visited states, never reaches zero. Spikes when the agent reaches genuinely new territory. | Reliable |
| `rnd/intrinsic_reward_mean` | Mean intrinsic reward across all envs at this step | Reliable |
| `rnd/intrinsic_reward_max` | Max intrinsic reward across all envs at this step. Spikes signal a novel state discovered. Cross-reference with `diagnostics/max_x_global`. | Reliable |

### `diagnostics/` (DiagnosticsCallback)
| Metric | Group | Description | Reliability |
|--------|-------|-------------|-------------|
| `diagnostics/max_x_episode` | A | Per-episode max x, averaged over rollout window | Reliable. Best spatial progress signal. |
| `diagnostics/max_x_global` | A | Env-0's lifetime `max_x_seen`, averaged | Reliable. Frontier tracker. |
| `diagnostics/steps_before_death` | B | **MISLEADINGLY NAMED.** Actually just episode length (`ep_len`). Logged for all episode types including truncations. | Reliable as ep_len; misleading as "before death" |
| `diagnostics/death_penalty_applied` | B | Fraction of episodes ending in death | Reliable |
| `diagnostics/died_before_obstacle` | B | Death episodes where max_x_ep < 85% of frontier | Reliable. Measures early-death rate. |
| `diagnostics/survival_bonus_total` | C | Cumulative survival bonus for the episode | Reliable |
| `diagnostics/survival_fraction` | C | ep_len / MAX_STEPS | Reliable |
| `diagnostics/episode_end_type` | — | 0=death, 1=truncation, 2=complete (mean over window) | Readable as centroid; prefer individual rate metrics |
| `diagnostics/truncation_rate` | — | Fraction of episodes truncated | Reliable |
| `diagnostics/level_complete_rate` | — | Fraction of episodes completed | Reliable. Key success signal. |
| `diagnostics/ent_coef_current` | D | ent_coef value every step (duplicates `train/ent_coef_scheduled`) | Reliable but redundant |
| `diagnostics/policy_entropy` | D | `-train/entropy_loss` (stale from last update, not current rollout) | **Stale.** Approximate. |
| `diagnostics/value_at_start_state` | E | V(s_0) evaluated every 10,000 steps | Reliable if `start_obs` was captured. Shows learning progress clearly. |
| `diagnostics/intrinsic_reward_episode_mean` | F | Mean per-step intrinsic reward for the episode (only logged when RND active) | Reliable. Key RND health signal. |
| `diagnostics/intrinsic_extrinsic_ratio` | F | `total_intrinsic / total_extrinsic` for the episode. Target: 0.3–1.0. See tuning guide below. | Reliable. Primary RND tuning signal. |
| `diagnostics/frontier_bonus_blocked_rate` | G | Fraction of steps per episode where Mario was airborne and would have set a new max_x but the bonus was gated. Healthy: 10–30%. | Reliable. Primary verification that the on-ground fix (Bug 4, 2026-03-20) is working. |
| `diagnostics/max_x_on_ground` | G | Highest world_x achieved while Mario was on the ground this episode. Excludes airborne positions. | Reliable. True navigable progress metric, complementary to max_x_episode. |
| `diagnostics/cells_visited_episode` | H | New (cx,cy) tile cells discovered this episode. Should be non-zero while the agent is exploring new territory. | Reliable. Episode-level exploration rate. |
| `diagnostics/cells_visited_total` | H | Lifetime total cells visited across all episodes. **Primary exploration progress metric** — replaces max_x_global as the frontier tracker. Should grow monotonically; flat means agent is looping in known territory. | Reliable. |
| `diagnostics/cells_per_step` | H | `episode_new_cells / ep_len`. Exploration efficiency. Healthy range: 0.01–0.05. High early in training, should decline as agent revisits territory. | Reliable. |
| `diagnostics/visited_cells_count` | H | Actual archive size from `len(env.visited_cells)` via `get_attr` (logged every `check_interval` steps). Monotonically non-decreasing by design. | Reliable. |
| `diagnostics/stomps_this_episode` | I | Mean stomps per episode (from `info["stomps_this_episode"]`). Should be non-zero once the agent reaches the goomba platform in 1-3. Zero with `--no-stomp`. | Reliable. |
| `diagnostics/sticky_action_rate` | I | Fraction of steps where the previous action was repeated. Expected value ≈ `sticky_prob` (0.25). Zero with `--no-sticky`. | Reliable. |
| `diagnostics/climbs_this_episode` | I | Mean forward+higher landings per episode. Non-zero once the agent is jumping onto elevated platforms. Zero with `--no-climb`. | Reliable. |

---

## Known Bugs & History

### Bug 1: `NewMaxXWrapper` used wrong info key — `x_pos` instead of `world_x`
**Status: FIXED** (current code confirmed correct)

**What was wrong:** `NewMaxXWrapper.step()` read `info.get("x_pos", 0.0)`. The environment emits `info["world_x"]`, not `info["x_pos"]`. `x_pos` never exists in info. This caused `max_x_seen` to permanently stay at `0.0`, making the frontier bonus always `0.0`. The wrapper was silently present but completely non-functional.

**Fix:** Changed to `info.get("world_x", 0.0)`. Confirmed fixed at `wrappers.py:89`.

**Impact of the bug:** Any training run that used the wrapper before this fix received zero frontier bonus for the entire run. The `diagnostics/max_x_global` metric would have been stuck at 0.0 (or never fired above 0).

**DO NOT reintroduce `info["x_pos"]` anywhere in the codebase.** It does not exist.

---

### Bug 2: Level selection in training script
**Status: FIXED** (git commit `bdd912a` — "fix level selection in training")

**What was wrong:** Unknown from code alone — the fix is in commit `bdd912a`. Likely related to incorrect level sampling or argument parsing for the `--levels` flag. The level injection mechanism (`_warm_reset`) is complex (writes `$0750`, `$0760`, `$075F`, `$075C` and forces `$0772=0` to re-trigger `LoadAreaPointer`) and was likely not plumbed correctly.

---

### Bug 3: `train_smb_rl.py` print statement reports `max_x_seen_init=400` — STALE
**Status: Outstanding (documentation-only bug)**

**What is wrong:** The startup print at line 213 says `NewMaxXWrapper(scale=2.0, max_x_seen_init=400)`. The actual initial value in `NewMaxXWrapper.__init__` is `self.max_x_seen: float = 0.0`. The print is wrong. `max_x_seen` starts at `0.0`, not `400`.

**Impact:** The health-check print at training start (`max_x_seen: {_cur_max_x}  (env 0)`) will show the correct value `0.0`, which contradicts the wrapper stack description line. Trust the health check, not the description line.

---

### Bug 4: `NewMaxXWrapper` fired frontier bonus during void jumps — airborne x not gated
**Status: FIXED 2026-03-20**

**What was wrong:** `NewMaxXWrapper.step()` updated `max_x_seen` and awarded the frontier bonus whenever `world_x > max_x_seen`, regardless of Mario's airborne/grounded state. When Mario jumped rightward into a pit, `world_x` at the airborne peak (or during the fall) could exceed the previous `max_x_seen`, triggering the frontier bonus for an x position that was physically unreachable at ground level. This directly incentivized void jumps — the agent learned that jumping into pits produced frontier reward, rather than learning to navigate the ground-level path.

**Fix:** `NewMaxXWrapper.step()` now gates both the bonus and the `max_x_seen` update on `info["on_ground"]` (True = Mario on ground). Ground state source: `smb_env.py` reads `RAM[0x001D] == 0x00` and adds `info["on_ground"]` to the step info dict. `RAM[0x001D]`: 0x00 = on ground, 0x01 = airborne (covers both deliberate jumps AND pit falls), 0x03 = level complete. Confirmed via single-frame RAM diff on 2026-03-20. `RAM[0x001C]` (the previously documented address) is always 0 in this emulator — do not use it. Only ground-level x positions count toward the lifetime frontier.

**New info keys added by the fix:**
- `info["mario_on_ground"]` (bool) — True when Mario is on the ground this step
- `info["frontier_bonus_blocked"]` (bool) — True when airborne prevented what would have been a new-max-x bonus

**New TensorBoard metrics added:**
- `diagnostics/frontier_bonus_blocked_rate` — fraction of steps in episode where bonus was gated by airborne state. Healthy range: 10–30%. Near 0% means Mario rarely reaches new ground while airborne (or fix not working). >50% means ground detection may be broken.
- `diagnostics/max_x_on_ground` — highest world_x achieved while Mario was on the ground this episode. This is the real navigable progress, distinct from `max_x_episode` which can include airborne positions.

**Impact of the bug:** All training runs before 2026-03-20 that used `NewMaxXWrapper` with `max_x_seen` initialized to 905 were optimizing the wrong objective near the World 1-3 goomba platform. The frontier reward was partially incentivizing running off cliffs and platform edges, not navigating the ground path. The first training run after this fix should show a change in the agent's risk profile near frontier obstacles.

---

### Finding (not a bug, but worth knowing): base env has its own alive bonus
`_compute_reward()` adds `+0.002` unconditionally every step (line 1227). This is separate from `SurvivalBonusWrapper`'s `+0.02`. Both are active. Combined per-step alive bonus is `+0.022`. The base env's bonus is small but non-zero.

---

## Entropy & Exploration System

`EntropySchedulerCallback` detects reward plateaus and boosts entropy to escape local optima.

**What it watches:** `rollout/ep_rew_mean` from `model.logger.name_to_value`. Appends the current value to a rolling window of size 5 at every step. Checks every 10,000 steps.

**Plateau trigger:** If `max(window) - min(window) < 0.5` (spread below threshold), it fires.

**What it does:** Sets `model.ent_coef = 0.05` (restart value), then decays it multiplicatively by `0.9995` each step until it reaches `floor_ent_coef = 0.001`, at which point `_restarting` is set to False.

**Current parameters (all defaults):**
| Parameter | Value |
|-----------|-------|
| `check_interval` | 10,000 steps |
| `plateau_threshold` | 0.5 |
| `restart_ent_coef` | 0.05 |
| `decay_rate` | 0.9995 |
| `floor_ent_coef` | 0.001 |
| `window_size` | 5 |

**Known issue:** `ep_rew_mean` is dominated by death penalty and survival bonus. A policy stuck at a local optimum (dying at a fixed point every episode) will produce a very stable `ep_rew_mean`, which *correctly* triggers plateau detection. However, a policy that is making genuine but slow progress may also have stable `ep_rew_mean` and trigger unnecessary restarts. The spread threshold of 0.5 is coarse — a 50-reward range can be maintained even with meaningful improvement.

**Metric logged:** `train/ent_coef_scheduled` every step. `DiagnosticsCallback` independently logs `diagnostics/ent_coef_current` (same value, different key).

---

## Training Configuration

### PPO Hyperparameters (hardcoded in `train_smb_rl.py`)

| Parameter | Value |
|-----------|-------|
| `policy` | `MultiInputPolicy` |
| `n_steps` | 1024 |
| `batch_size` | 256 |
| `n_epochs` | 4 |
| `learning_rate` | 3e-4 |
| `gamma` | 0.99 |
| `gae_lambda` | 0.95 |
| `clip_range` | 0.2 |
| `ent_coef` | 0.02 (initial; overridden by EntropyScheduler) |
| `vf_coef` | 0.5 |
| `max_grad_norm` | 0.5 |
| `net_arch` | `pi=[256, 256], vf=[256, 256]` |

### Checkpoint Situation

No hardcoded checkpoint path. Controlled by `--checkpoint-dir` (default: `checkpoints/`). Checkpoints are saved:
- Every `--eval-interval` steps (default: 100,000) as `checkpoints/model_{step:010d}.zip`
- At training end as `checkpoints/model_{step:010d}_final.zip`
- A `curriculum.json` is written alongside checkpoints with full curriculum metadata.

To resume: `python rl/train_smb_rl.py --rom ... --resume checkpoints/model_XXXXXXXXXX.zip`. When resuming, `reset_num_timesteps=False` so the step counter continues from where it left off.

### max_x_seen Initialization

`NewMaxXWrapper.max_x_seen` initializes to `0.0` at construction time. In `train_smb_rl.py`, immediately after `SubprocVecEnv` is created, the frontier is set to the known prior value via:

```python
vec_env.env_method("__setattr__", "max_x_seen", 905)
```

This pre-loads the lifetime frontier to **905** (the furthest x reached in prior runs). Without this, the frontier bonus fires everywhere below x=905 on the first episode, producing a misleading spike in `max_x_global` and incorrect `died_before_obstacle` thresholds. When starting a genuinely fresh run, remove this line or set it to 0.

Each of the 8 workers has its own independent `max_x_seen`. There is no cross-worker sharing — env 0's value is used as the representative global frontier in diagnostics.

### Default Training Invocation

```bash
python rl/train_smb_rl.py --rom roms/smb1.nes
# Trains 10M steps, 8 envs, level 1-1 only, eval every 100k steps
```

Two-stage curriculum example:
```bash
python rl/train_smb_rl.py --rom roms/smb1.nes \
    --levels 1-1 1-2 --level-weights 2 1 \
    --stage1-steps 2_000_000 \
    --stage2-levels 1-1 1-2 1-3 1-4 --stage2-weights 4 3 2 1
```

---

## Architecture

- **Policy:** `MultiInputPolicy` (SB3). Handles `Dict` observation spaces via separate encoders per key, then concatenates into shared MLP trunk.
- **Network:** MLP-only (no CNN). Observations are structured RAM/PPU features, not pixels. Actor and critic each use `[256, 256]` fully-connected layers.
- **Observation modality:** Pure RAM + PPU nametable features. `FRAME_SKIP = 3` (3 NES frames per env step). Actions are multi-frame motor primitives (5–10 NES frames each).
- **Emulator backend:** `libmicrones_rl.{dylib,so}` — headless training lib. `libmicrones_rl_render.{dylib,so}` — framebuffer-enabled eval lib. Both are compiled from the same `src/common` NES core. Build target: `cmake --build build-host -j`.
- **Parallelism:** `SubprocVecEnv` with 8 workers (default). Each worker is a fully independent NES emulator instance.
- **Frame recording:** Only when `render_mode="rgb_array"`. Training envs use `render_mode=None`. Eval envs (in `EvalVideoCallback` and `record_level.py`) use the render lib and capture per-emulator-frame via `env.pop_step_frames()`.

---

## What Has Been Tried (Experiment History)

**Note:** Git history has only 5 RL-related commits. The following is reconstructed from `# BUGFIX` annotations, inline comments, and audit findings.

### 1. Initial RL implementation (`feea6e2`)
First PPO agent. Likely single-level (1-1), basic reward structure. Details unknown from code alone.

### 2. Level selection broken, then fixed (`bdd912a`)
At some point the curriculum level sampling or level injection was incorrect. Fixed in commit `bdd912a`. The level injection mechanism is fragile — it requires writing 4 RAM addresses plus forcing `$0772=0` while re-asserting the values for 60 frames. If any of those writes were missing or in the wrong order, the agent would train on 1-1 regardless of the configured level.

### 3. New actions and record_level tool added (`511c509`)
Expanded action space (current: 14 actions including crouch, fireball). Added `record_level.py` for checkpoint evaluation. Before this, there was presumably a smaller action set.

### 4. NewMaxXWrapper frontier bonus broken (unknown commit)
The `NewMaxXWrapper` was reading `info.get("x_pos", 0.0)` instead of `info.get("world_x", 0.0)`. The env only emits `info["world_x"]`. This caused `max_x_seen` to be stuck at `0.0` for the entire training run, making the frontier bonus zero. The fix annotation says this was a pre-existing bug found during the death-detection audit on 2026-03-19.

**Lesson:** When adding a new info key or renaming one, search for all consumers of that key across all wrappers and callbacks. The mismatch was silent — no exception, just a permanently zero reward component.

### 5. DeathPenaltyWrapper redesign
Earlier version of `DeathPenaltyWrapper` replaced the entire reward on death with `-death_penalty` and stripped the x-progress component. The rationale was to avoid the "void-jump problem" — an agent that learns to sprint right and die far away, collecting x-reward on the death step. The current version is additive instead (adds `-4.0` on top of the base env's `-10.0`). The "void-jump" problem is now handled by the `NewMaxXWrapper` frontier design: dying at a previously-reached x yields zero frontier bonus, since `max_x_seen` already covers that x.

---

## Random Network Distillation (RND)

### Architecture

RND uses two MLP networks (no CNN — observations are structured features, not pixels):
- **Target**: `Linear(371→512) → LeakyReLU → Linear(512→512) → LeakyReLU → Linear(512→512)`, orthogonal init, **frozen permanently**.
- **Predictor**: Identical architecture, **trained** by `RNDTrainerCallback` after each rollout.

Input dim is computed dynamically from the flat observation space (`flat_obs_dim_from_space`). Currently ≈ 371. Do not hardcode this number.

### How intrinsic reward is computed (per step, in `RNDWrapper`)

1. Flatten Dict obs to a 1-D float32 vector (keys sorted for determinism).
2. Update `obs_rms` (per-instance `RunningMeanStd(shape=(371,))`).
3. Normalise: `clip((obs - mean) / std, -5, 5)`.
4. Forward through frozen target and predictor (both `torch.no_grad()`).
5. `raw_intrinsic = MSE(pred_feat, target_feat)`.
6. Update `reward_rms` with discounted intrinsic return `_ret = _ret * 0.99 + raw_intrinsic`.
7. `intrinsic_norm = raw_intrinsic / sqrt(reward_rms.var + 1e-8)`.
8. `total_reward = extrinsic + intrinsic_scale * intrinsic_norm`.

### Non-episodic intrinsic reward

`_ret` (the discounted return used for reward normalisation) is **not reset on episode end** by default (`episodic_intrinsic=False`). This is the correct RND setting. Mario dying should not reset curiosity — the agent still needs to get past the same obstacle next episode.

### Subprocess weight synchronisation

Each `SubprocVecEnv` worker has its own copy of the RND networks (created at fork time). The main process trains `rnd_predictor` in `RNDTrainerCallback._on_rollout_end`, then calls `env_method("sync_rnd_predictor", weights)` to push the updated weights to all workers. Without this sync, workers would always use the initial random predictor, producing uniform-high intrinsic rewards (no novelty signal).

The sync uses numpy serialisation (`state_dict().cpu().numpy()`) for cloudpickle compatibility over SubprocVecEnv's IPC.

### RND tuning guide

Monitor `diagnostics/intrinsic_extrinsic_ratio`:
- `> 2.0` → intrinsic dominates; reduce `--rnd-scale` (e.g. 1.0 → 0.5)
- `< 0.1` → intrinsic too weak; increase `--rnd-scale` (e.g. 1.0 → 2.0)
- `0.3–1.0` → healthy balance; leave it

Monitor `rnd/predictor_loss`:
- Should start high (predictor knows nothing)
- Should decrease across rollouts (predictor learns visited states)
- Should never reach zero (novel states always exist)
- Should spike when the agent reaches new territory

If `max_x_episode` stops growing but `rnd/intrinsic_reward_mean` is high: the agent is exploring for curiosity's own sake. Reduce `--rnd-scale` by 50%.

### Files

| File | Purpose |
|------|---------|
| `rl/rnd.py` | `RNDNetwork`, `make_rnd_networks`, `flatten_obs_single`, `flatten_obs_batch`, `flat_obs_dim_from_space`, `warmup_rnd_obs_normalizer` |
| `rl/running_mean_std.py` | `RunningMeanStd` — Welford online mean/variance, used for obs and reward normalisation |

---

## DO NOT

1. **Do not reset `visited_cells` on episode end.** `VisitedCellsWrapper.reset()` intentionally does not reset `self.visited_cells`. Resetting it would re-reward familiar territory every episode, destroying the curriculum pressure. See the wrapper docstring for the rationale.

2. **Do not reset `max_x_seen` on episode end.** `NewMaxXWrapper.reset()` intentionally does not reset `self.max_x_seen`. If you reset it, the frontier bonus fires on familiar ground every episode, destroying the curriculum pressure it provides. The comment in the code explicitly warns against this.

2. **Do not use `info["x_pos"]` or `info["death"]`.** These keys do not exist. Death is `obs["game_flags"][0] > 0.5`. x position is `info["world_x"]`. Adding logic that reads either of these non-existent keys will silently produce 0 / False without raising an exception.

3. **Do not use `rollout/ep_rew_mean` as the primary plateau or progress signal.** It is dominated by the death penalty and is not a reliable indicator of whether the agent is improving spatially. Use `diagnostics/max_x_episode` and `diagnostics/level_complete_rate` instead.

4. **Do not reintroduce stripping x-reward on the death step.** This was the old `DeathPenaltyWrapper` design and was explicitly abandoned. Without the frontier bonus being correctly functional, stripping x-reward on death can cause reward collapse: the agent gets no positive signal near the hardest part of the level. The current design (additive penalty + frontier bonus) is intentional.

5. **Do not call `DeathPenaltyWrapper` with `death_penalty=10.0` assuming it matches the base env.** The base env already applies `-10.0` inside `_compute_reward()`. The wrapper's penalty is **additive**. With `death_penalty=10.0`, total on-death penalty would be `-20.0`, which is catastrophic relative to the reward scale. Default is `4.0` for a reason.

6. **Do not add pixel-based observations or a CNN policy.** Observations are structured RAM features. `MultiInputPolicy` handles the Dict obs via separate MLP encoders. Switching to a CNN requires rebuilding the entire observation pipeline.

7. **Do not evaluate using the training lib (`libmicrones_rl`).** The training lib has the framebuffer disabled. `env.pop_step_frames()` will return empty lists. Eval video recording requires the render lib (`libmicrones_rl_render`). `EvalVideoCallback` auto-detects the render lib; `record_level.py` also auto-detects it.

8. **Do not reset `obs_rms` or `reward_rms` in `RNDWrapper.reset()`.** These are lifetime statistics. Resetting them on episode end destroys the normalisation calibration and causes intrinsic reward spikes at every episode boundary.

9. **Do not call `rnd_target.train()` anywhere.** The target network must stay in eval mode and have no parameter updates. It is frozen in `make_rnd_networks()` (`requires_grad=False` + `target.eval()`). Any accidental `target.train()` call re-enables dropout/batchnorm training behaviour (if those layers were added in future).

10. **Do not hardcode the flat observation dimension (371).** Compute it dynamically via `flat_obs_dim_from_space(env.observation_space)`. If the observation space gains new keys, the hardcoded value would silently produce wrong-shaped tensors.

11. **Do not skip `sync_rnd_predictor` after training the predictor.** Without the sync, each worker subprocess uses the original (random) predictor indefinitely, producing uniformly high intrinsic rewards with no novelty signal. The sync is called automatically by `RNDTrainerCallback._on_rollout_end`.

12. **Do not place `RNDTrainerCallback` after other callbacks in the list.** It must be first so the predictor is trained and synced before the next rollout begins. Order matters — the DiagnosticsCallback reads intrinsic reward from the info dict that RNDWrapper has already modified.

---

## Open Questions

1. **What is `max_x_seen` in practice after a 10M-step run?** There is no logged checkpoint of a completed run in the repo. The frontier pressure of `NewMaxXWrapper` depends on `max_x_seen` being realistic. If the agent never advances past x≈200, the frontier bonus is permanently zero past that point.

2. **Is the `EntropySchedulerCallback` plateau threshold (0.5) appropriate for this reward scale?** Episodes with a single death penalty swing `ep_rew_mean` by ≈14 units. A window spread of < 0.5 requires very consistent episode outcomes. The threshold may be too tight (triggers too often) or too loose depending on batch size and number of envs.

3. **Are there any cross-env `max_x_seen` consistency issues?** Each of the 8 SubprocVecEnv workers has an independent `NewMaxXWrapper` with its own `max_x_seen`. Env 0's frontier is used as the global representative in `DiagnosticsCallback`. If env 0 gets unlucky early, the `died_before_obstacle` threshold may be calibrated against a stale frontier.

4. **`diagnostics/steps_before_death` is misleadingly named.** It is episode length (`ep_len`), logged for all episode types. A rename to `diagnostics/episode_length` would be clearer, but the change would break any existing TensorBoard dashboards or scripts that reference the old name.

5. **What happens to `route_viability` observations in early training?** The viability score drives both reward shaping and observation features. If the topology scan consistently returns 0 (no reachable surfaces found), the viability-attenuated x-reward will be reduced by `viability_factor = 0.2`, which may slow early learning. This has not been empirically measured.

---

## Files in `rl/`

| File | Purpose |
|------|---------|
| `smb_env.py` | Base Gymnasium environment. All observation extraction, reward, termination logic. |
| `wrappers.py` | `NewMaxXWrapper`, `SurvivalBonusWrapper`, `DeathPenaltyWrapper`, `RNDWrapper`. Full BUGFIX audit at top of file. |
| `eval_callback.py` | `DiagnosticsCallback`, `EntropySchedulerCallback`, `CurriculumStageCallback`, `EvalVideoCallback`, `RNDTrainerCallback`. |
| `train_smb_rl.py` | Training entry point. PPO config, wrapper assembly, callback assembly. `--no-rnd` to disable RND, `--rnd-scale` to tune. |
| `rnd.py` | `RNDNetwork` (MLP), `make_rnd_networks`, obs-flattening helpers, `warmup_rnd_obs_normalizer`. |
| `running_mean_std.py` | `RunningMeanStd` — Welford online mean/variance for obs and reward normalisation. |
| `record_level.py` | Record deterministic eval episodes as MP4 from a saved checkpoint. |
| `nes_ctypes.py` | ctypes bindings for `libmicrones_rl`. Zero-copy numpy views into emulator RAM/PPU/OAM/framebuffer. |
| `diag_level_select.py` | Diagnostic tool: compares RAM snapshots between natural 1-1 boot and injected level boot. Used to debug level injection. |
| `requirements.txt` | Python dependencies: gymnasium>=0.29, stable-baselines3>=2.3, numpy>=1.24, imageio, tensorboard, tqdm, rich. |
| `micrones_rl.h` / `micrones_rl.c` | C interface for the RL shared library (headless). |
