# Papers

Reference papers for the MicroNES RL training project. These are kept in the
repo so Claude Code and other tools can read them directly during implementation.

---

## go-explore-nature-2021.pdf

**Title:** First Return, Then Explore  
**Authors:** Adrien Ecoffet, Joost Huizinga, Joel Lehman, Kenneth O. Stanley, Jeff Clune  
**Published:** Nature, Vol. 590, pp. 580–586, February 2021  
**DOI:** https://doi.org/10.1038/s41586-020-03157-9  
**arXiv:** https://arxiv.org/abs/2004.12919  
**Source:** Author's personal copy hosted at adrien.ecoffet.com (legitimate free distribution)

**What it is:**  
The full peer-reviewed paper introducing Go-Explore, a family of RL algorithms
for hard-exploration problems. Go-Explore solved all previously unsolved Atari
hard-exploration games and set superhuman records on Montezuma's Revenge and
Pitfall — two games with the same structural characteristics as Super Mario Bros
World 1-3 (sparse rewards, deceptive feedback, precision timing required).

**Why it's here:**  
We are implementing Go-Explore for MicroNES/SMB1 as the next major training
strategy after PPO+RND hit a hard wall at x≈905 in World 1-3 (the goomba
platform obstacle). Go-Explore directly addresses the two failure modes we
observed: detachment (agent forgets the frontier) and derailment (agent can't
reliably return to promising states).

**Key sections for implementation:**
- Figure 1 — algorithm overview, the two-phase structure
- Algorithm 1 — Phase 1 pseudocode (exploration loop)
- Algorithm 2 — Phase 2 pseudocode (robustification via imitation learning)
- Section "Cell representation" — how states are compressed into cells
- Extended Data Table 1 — hyperparameters used for Atari experiments
- Section "Domain knowledge" — the high-performing variant using hand-designed
  cell features (x/y position, room number) rather than downscaled pixels

**Implementation decisions already made for this project:**

1. **Cell representation:** We use RAM observations (≈371 floats including
   world_x, mario_y, enemy positions). Cell key = discretized (world_x, mario_y)
   to tile granularity. Do NOT implement the downscaled pixel cell approach —
   our observation space is structured RAM, not pixels.

2. **Return mechanism:** MicroNES has no save-state API. Return to archived
   cells must use action replay (replay stored action sequence from episode
   start). The emulator is deterministic so this is reliable. Do NOT implement
   the save-state return path.

3. **Phase 2 (robustification):** Skip for initial implementation. The MicroNES
   environment is deterministic during training, so Phase 1 demonstrations may
   be sufficient. Phase 2 can be added later if stochasticity at eval time
   becomes a problem.

4. **Base RL algorithm:** PPO (Stable-Baselines3) remains the policy optimizer
   for Phase 2 if/when implemented. Do not switch algorithms.

5. **Observation keys:** The environment exposes `info["world_x"]` (not
   `info["x_pos"]` — this was a bug fixed early in the project). Mario y
   position is available from the RAM observation vector directly.

---

## IMPLEMENTATION_NOTES.md (this file)

See below for running notes on Go-Explore implementation decisions, discovered
during development. Update this as implementation progresses.

### Cell granularity
The paper uses 8x8 pixel downscaling for Atari pixel observations. For our
RAM-based observations, tile granularity of 16 world_x units × 8 mario_y units
is a reasonable starting point. This gives ~60 x-cells across a typical SMB1
level and ~24 y-cells for vertical range. Too coarse = archive never grows.
Too fine = archive grows forever with no useful prioritization. Start at 16×8
and tune based on `go_explore/archive_size` growth curve.

### Archive selection
Do NOT use uniform random cell selection — the paper's weighted selection
(favoring high-x, low-visit-count cells) is critical for performance. Implement
the exact formula from the paper. Uniform selection works but is dramatically
slower to reach the frontier.

### What to store per cell
Each archive entry stores:
- The action sequence to reach this cell (for action replay return)
- Visit count
- Best score achieved from this cell
- The cell's x position (for prioritization and logging)

Store the SHORTEST action sequence that reaches each cell, not the first one
found. Shorter trajectories = faster returns = more exploration per unit time.

### Logging requirements
The following TensorBoard metrics must be implemented for Go-Explore:
- `go_explore/archive_size` — total cells in archive (must grow monotonically)
- `go_explore/max_x_cell` — highest world_x cell in archive
- `go_explore/return_success_rate` — fraction of returns landing at target cell
- `go_explore/new_cells_per_explore` — avg new cells found per explore episode
- `go_explore/explore_steps` — total steps spent in exploration phase

### Known failure modes to test for
1. Flat `archive_size` → cell function too coarse or return mechanism broken
2. `return_success_rate` < 80% → action replay is unreliable (check emulator
   determinism)
3. `max_x_cell` never exceeds PPO's x=905 → archive selection is broken or
   cell granularity wrong
4. `new_cells_per_explore` = 0 → random exploration isn't generating variety

---

## papers not yet added

The following papers are referenced in the project but not yet downloaded:

- **go-explore-2019-preprint.pdf** — The original 2019 arXiv preprint
  (arxiv:1901.10995). Contains some supplementary Atari implementation detail
  not in the final Nature paper. Low priority since the 2021 paper is complete.

- **rnd-2018.pdf** — "Exploration by Random Network Distillation" (Burda et al.
  2018). The paper behind our current RND implementation. Useful reference if
  RND behavior needs debugging. arXiv: https://arxiv.org/abs/1810.12894

- **large-scale-curiosity-2018.pdf** — "Large-Scale Study of Curiosity-Driven
  Learning" (Burda et al. 2018). The OpenAI paper that got closest to solving
  SMB1 via pure curiosity, reaching World 3-4 via warp pipe.
  arXiv: https://arxiv.org/abs/1808.04355
