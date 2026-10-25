"""Deterministic unit tests for the provisional SteadyStep rule engine."""
import sys
import unittest
from pathlib import Path
import pandas as pd

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from run_validation import run_rules  # noqa: E402


def make_patient(weight_series, hr_series, impedance_series=None, bad_weight_days=None):
    n = len(weight_series)
    impedance_series = impedance_series or [600.0] * n
    bad_weight_days = set(bad_weight_days or [])
    rows = []
    for day in range(n):
        rows.append({
            "patient_id": "TEST01",
            "day_index_0_based": day,
            "date": f"2026-01-{day+1:02d}",
            "weight_kg": weight_series[day],
            "resting_hr_bpm": hr_series[day],
            "foot_impedance_ohm": impedance_series[day],
            "weight_signal_quality": "missing" if day in bad_weight_days else "valid",
            "heart_rate_signal_quality": "valid",
            "impedance_signal_quality": "valid",
        })
    return pd.DataFrame(rows)


class SteadyStepRuleTests(unittest.TestCase):
    def test_weight_only_persistent_change_becomes_watch(self):
        # Seven baseline sessions, then two consecutive weight elevations.
        df = make_patient([70]*7 + [72]*3, [70]*10)
        out = run_rules(df)
        self.assertEqual(out.iloc[-1].alert_state, "WATCH")
        self.assertTrue(bool(out.iloc[-1].weight_persistent))
        self.assertFalse(bool(out.iloc[-1].heart_rate_persistent))

    def test_both_persistent_changes_become_elevated_pattern(self):
        df = make_patient([70]*7 + [72]*3, [70]*7 + [82]*3)
        out = run_rules(df)
        self.assertEqual(out.iloc[-1].alert_state, "ELEVATED_PATTERN")
        self.assertTrue(bool(out.iloc[-1].weight_persistent))
        self.assertTrue(bool(out.iloc[-1].heart_rate_persistent))

    def test_no_trigger_is_not_called_normal_when_impedance_unverified(self):
        df = make_patient([70]*10, [70]*10)
        out = run_rules(df)
        self.assertEqual(out.iloc[-1].alert_state, "INSUFFICIENT_DATA")
        self.assertFalse(bool(out.iloc[-1].rule_alert))

    def test_bad_quality_weight_does_not_count_as_valid_observation(self):
        df = make_patient([70]*10, [70]*10, bad_weight_days={0, 1, 2})
        out = run_rules(df)
        self.assertFalse(bool(out.iloc[2].weight_valid))
        self.assertTrue(bool(out.iloc[-1].heart_rate_valid))

    def test_rule_engine_rejects_label_leakage(self):
        df = make_patient([70]*10, [70]*10)
        df["worsening_period_label_EVALUATION_ONLY"] = 0
        with self.assertRaises(ValueError):
            run_rules(df)


if __name__ == "__main__":
    unittest.main(verbosity=2)
