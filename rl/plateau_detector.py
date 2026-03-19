"""
Per-level rolling-window plateau detector for SMB1 RL training.

Usage:
    cfg = PlateauConfig(window=8, min_max_x_gain=50)
    det = PlateauDetector(cfg)
    result = det.record("1-3", step=1_000_000, metrics=metrics_dict)
    if result["plateau"]:
        print(result["plateau_reason"])
    if result["should_stop"]:
        # save checkpoint and stop
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass


@dataclass
class PlateauConfig:
    """Configuration for plateau detection thresholds."""

    # Number of consecutive eval snapshots to look back over.
    window: int = 8

    # Minimum improvement in mean max_x over the window to NOT be plateaued.
    min_max_x_gain: float = 50.0

    # Minimum improvement in passed_opening_rate over the window.
    min_passed_opening_gain: float = 0.05

    # Minimum improvement in success_rate over the window.
    min_success_rate_gain: float = 0.02

    # fall_death_rate >= this → classify as "void_failure_dominant".
    fall_death_dominant_threshold: float = 0.3

    # Stop training after this many consecutive plateau windows (0 = disabled).
    stop_patience: int = 0


class PlateauDetector:
    """
    Maintains per-level metric history and detects training plateaus.

    Plateau is flagged when, over the last `window` eval checkpoints:
      - max_x_mean improvement < min_max_x_gain  AND
      - passed_opening_rate improvement < min_passed_opening_gain  AND
      - success_rate improvement < min_success_rate_gain

    When plateaued, classifies the reason as one of:
      void_failure_dominant     — fall_death_rate still high
      same_region_repeat_failure — agent always dies in same small x range
      no_forward_progress        — max_x_mean is very low (< 200)
      no_opening_breakthrough    — never gets past opening threshold (< 10%)
      generic_plateau            — progress stalled but no specific cause

    If stop_patience > 0 and consecutive plateau count >= stop_patience,
    sets should_stop=True so the training loop can terminate cleanly.
    """

    def __init__(self, config: PlateauConfig | None = None):
        self._cfg = config or PlateauConfig()
        # history[level] = deque of (step, metrics_dict), max size = window+1
        self._history: dict[str, deque] = {}
        # consecutive plateau count per level
        self._consecutive: dict[str, int] = {}

    def record(self, level: str, step: int, metrics: dict) -> dict:
        """
        Record a new eval snapshot and return plateau analysis.

        Args:
            level:   Level string, e.g. "1-3".
            step:    Training step at which eval occurred.
            metrics: Dict with keys: max_x_mean, passed_opening_rate,
                     success_rate, fall_death_rate, death_x_std, etc.

        Returns dict with:
            plateau:              bool
            plateau_reason:       str or None
            consecutive_plateaus: int
            should_stop:          bool
        """
        if level not in self._history:
            self._history[level] = deque(maxlen=self._cfg.window + 1)
            self._consecutive[level] = 0

        self._history[level].append((step, dict(metrics)))

        # Need at least `window` entries to evaluate a full window.
        if len(self._history[level]) < self._cfg.window:
            return {
                "plateau":              False,
                "plateau_reason":       None,
                "consecutive_plateaus": 0,
                "should_stop":          False,
            }

        plateau, reason = self._check(level)

        if plateau:
            self._consecutive[level] += 1
        else:
            self._consecutive[level] = 0

        consec = self._consecutive[level]
        should_stop = (
            self._cfg.stop_patience > 0 and consec >= self._cfg.stop_patience
        )

        return {
            "plateau":              plateau,
            "plateau_reason":       reason,
            "consecutive_plateaus": consec,
            "should_stop":          should_stop,
        }

    def _check(self, level: str) -> tuple[bool, str | None]:
        """Check if oldest→newest in the window shows insufficient progress."""
        hist = list(self._history[level])
        cfg  = self._cfg
        old  = hist[0][1]
        new  = hist[-1][1]

        max_x_gain   = new.get("max_x_mean",          0.0) - old.get("max_x_mean",          0.0)
        passed_gain  = new.get("passed_opening_rate",  0.0) - old.get("passed_opening_rate",  0.0)
        success_gain = new.get("success_rate",         0.0) - old.get("success_rate",         0.0)

        no_progress = (
            max_x_gain   < cfg.min_max_x_gain           and
            passed_gain  < cfg.min_passed_opening_gain   and
            success_gain < cfg.min_success_rate_gain
        )

        if not no_progress:
            return False, None

        # Plateau confirmed — classify reason.
        fall_rate   = new.get("fall_death_rate",  0.0)
        death_x_std = new.get("death_x_std",      1e9)
        max_x       = new.get("max_x_mean",       0.0)
        passed_rate = new.get("passed_opening_rate", 0.0)

        if fall_rate >= cfg.fall_death_dominant_threshold:
            return True, "void_failure_dominant"
        if death_x_std < 30.0 and max_x < 1000.0:
            return True, "same_region_repeat_failure"
        if max_x < 200.0:
            return True, "no_forward_progress"
        if passed_rate < 0.1:
            return True, "no_opening_breakthrough"
        return True, "generic_plateau"
