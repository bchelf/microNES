"""
Per-level configuration for SMB1 RL training.

OPENING_THRESHOLDS[level]: world_x pixel value at which Mario is considered
to have passed the "opening section" of that level. Used to compute the
passed_opening_rate metric during eval.

LEVEL_CHECKPOINTS[level]: list of (StartMode, target_x, weight) tuples
defining available start-state families and their sampling weights.
Levels not listed default to FULL_LEVEL_START only.

ROUTE_VIABILITY_CONFIGS[level]: RouteViabilityConfig with thresholds for
computing route-viability observation features.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import IntEnum


# ---------------------------------------------------------------------------
# Opening thresholds
# ---------------------------------------------------------------------------

OPENING_THRESHOLDS: dict[str, int] = {
    "1-1": 600, "1-2": 600, "1-3": 600, "1-4": 200,
    "2-1": 600, "2-2": 600, "2-3": 600, "2-4": 200,
    "3-1": 600, "3-2": 600, "3-3": 600, "3-4": 200,
    "4-1": 600, "4-2": 600, "4-3": 600, "4-4": 200,
    "5-1": 600, "5-2": 600, "5-3": 600, "5-4": 200,
    "6-1": 600, "6-2": 600, "6-3": 600, "6-4": 200,
    "7-1": 600, "7-2": 600, "7-3": 600, "7-4": 200,
    "8-1": 600, "8-2": 600, "8-3": 600, "8-4": 200,
}

DEFAULT_OPENING_THRESHOLD = 600


# ---------------------------------------------------------------------------
# Start-state families
# ---------------------------------------------------------------------------

class StartMode(IntEnum):
    FULL_LEVEL_START             = 0  # normal level start from x=0
    PRE_COMMIT_START             = 1  # approaching the key irreversible decision
    COMMIT_WINDOW_START          = 2  # at/near the jump-timing window
    MISSED_WINDOW_RECOVERY_START = 3  # past ideal window, backward recovery still viable
    POST_SUCCESS_START           = 4  # already on successful route continuation


# Per-level checkpoint registry:
#   level -> list of (StartMode, target_x, weight)
# target_x=0: normal level start.
# target_x>0: fast-forward Mario to approximately that world_x via scripted movement.
# Weights are unnormalized; they are normalized at sample time.
#
# For levels NOT in this dict, only FULL_LEVEL_START is used.
#
# 1-3 checkpoint x-values (approximate; tune based on actual level playthrough):
#   The level's critical section is the platform sequence over the long gap.
#   Upper route requires a well-timed jump at ~x=650-750; lower route leads to death.
#   Exact values should be verified with emulator playthroughs.
LEVEL_CHECKPOINTS: dict[str, list[tuple[StartMode, int, float]]] = {
    "1-3": [
        (StartMode.FULL_LEVEL_START,               0, 0.40),
        (StartMode.PRE_COMMIT_START,             450, 0.18),
        (StartMode.COMMIT_WINDOW_START,          650, 0.18),
        (StartMode.MISSED_WINDOW_RECOVERY_START, 850, 0.12),
        (StartMode.POST_SUCCESS_START,          1050, 0.12),
    ],
}


# ---------------------------------------------------------------------------
# Route viability config
# ---------------------------------------------------------------------------

@dataclass
class RouteViabilityConfig:
    """
    Thresholds for computing route-viability observation features for a level.

    Fields:
        commit_window_x:        world_x at which the commitment window begins.
        commit_window_end_x:    world_x past which the commitment window is missed.
        recovery_impossible_x:  world_x past which backward recovery to upper route
                                is geometrically impossible (too far to backtrack).
        upper_route_screen_y:   screen_y at or below which Mario is on the upper route.
                                NES screen Y increases downward; upper = smaller value.
        lower_doom_screen_y:    screen_y at or above which Mario is on the lower/doomed
                                route after passing the commit window.
    """
    commit_window_x:       int = 0
    commit_window_end_x:   int = 0
    recovery_impossible_x: int = 0
    upper_route_screen_y:  int = 0   # <= this → on upper route
    lower_doom_screen_y:   int = 255  # >= this (when past commit) → doomed


# Per-level route viability thresholds.
# Only needed for levels with hard commitment motifs.
# Levels not listed return all-zero route_viability features.
#
# 1-3 approximate values (tune from gameplay data):
#   The upper platform cluster is at screen_y ~80-100 (NES Y is inverted from top).
#   The lower ground path that leads into the pit is at screen_y ~190-210.
ROUTE_VIABILITY_CONFIGS: dict[str, RouteViabilityConfig] = {
    "1-3": RouteViabilityConfig(
        commit_window_x       = 600,
        commit_window_end_x   = 750,
        recovery_impossible_x = 1050,
        upper_route_screen_y  = 120,
        lower_doom_screen_y   = 170,
    ),
}
