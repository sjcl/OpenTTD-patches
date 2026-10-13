"""Focused checks for evidence parsing, overflow handling and comparison gates."""
import importlib.util
import json
import pathlib
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("summary", pathlib.Path(__file__).with_name("Summarize-Profile.py"))
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)
rank_spec = importlib.util.spec_from_file_location("ranking", pathlib.Path(__file__).with_name("Rank-Profile.py"))
ranking = importlib.util.module_from_spec(rank_spec)
rank_spec.loader.exec_module(ranking)
instrument_spec = importlib.util.spec_from_file_location("instrument", pathlib.Path(__file__).with_name("Instrument-Profile.py"))
instrument = importlib.util.module_from_spec(instrument_spec)
instrument_spec.loader.exec_module(instrument)


class EvidenceTests(unittest.TestCase):
    def test_observed_calls_keep_short_circuit_and_nested_arguments(self):
        expression = 'ok && Station::GetIfValid((*graph)[node].Station()) != nullptr'
        result = instrument.observe_calls(expression, 'Station::GetIfValid', 'live.station_get_if_valid')
        self.assertEqual(result, 'ok && JourneyProfile::Observe("live.station_get_if_valid", Station::GetIfValid((*graph)[node].Station())) != nullptr')
        self.assertEqual(result.count('(*graph)[node].Station()'), 1)
    def fixture(self, label, kind='minimal', mode='on', duration=35):
        manifest = {"label": label, "compiler": {}, "package": {"input_sha256": "s", "asset_manifest_sha256": "a", "config_sha256": "c"},
                    "host": {"cpu": "cpu"}, "settings": {}, "kind": kind, "harness_sha256": "h", "phase": "initial", "cache_off_scope": "OD"}
        run = {"label": label, "kind": kind, "phase": "initial", "mode": mode, "id": mode + '-1', "valid": True,
               "diagnostics": {"state_ticks": [0, 8192]}, "derived": {"simulation_mean_ms": duration},
               "semantic_signature": {"work": 100}, "final_save_sha256": "save"}
        return manifest, [run]

    def test_detail_never_enters_speed_comparison(self):
        fixtures = {"baseline": self.fixture('baseline'), "A": self.fixture('A', duration=27), "detail": self.fixture('A', kind='detail', duration=500)}
        with patch.object(summary, 'read_manifest', side_effect=lambda p: fixtures[p.name]):
            result = summary.summarize([pathlib.Path(p) for p in fixtures], 'baseline')
        self.assertEqual(len(result['comparisons']), 1)
        self.assertTrue(result['comparisons'][0]['compatible'])
        self.assertAlmostEqual(result['comparisons'][0]['relative_change'], 27 / 35 - 1)

    def test_changed_host_refuses_speedup(self):
        fixtures = {"baseline": self.fixture('baseline'), "A": self.fixture('A', duration=27)}
        fixtures['A'][0]['host']['cpu'] = 'different'
        with patch.object(summary, 'read_manifest', side_effect=lambda p: fixtures[p.name]):
            result = summary.summarize([pathlib.Path(p) for p in fixtures], 'baseline')
        self.assertFalse(result['comparisons'][0]['compatible'])
        self.assertIsNone(result['comparisons'][0]['relative_change'])

    def test_cache_mismatch_is_reported(self):
        manifest, runs = self.fixture('baseline')
        cold = self.fixture('baseline', mode='cold')[1][0]
        cold['semantic_signature']['work'] = 101
        cold['final_save_sha256'] = 'different-save'
        with patch.object(summary, 'read_manifest', return_value=(manifest, runs + [cold])):
            result = summary.summarize([pathlib.Path('baseline')], 'baseline')
        self.assertFalse(result['cache_invariance'][0]['full_save_identical'])
        self.assertFalse(result['cache_invariance'][0]['work_transport_outcomes_identical'])

    def test_missing_or_detailed_load_has_no_rank(self):
        self.assertEqual(ranking.rank({'runs': []})['status'], 'not_measured')
        run = self.fixture('baseline', kind='detail')[1][0]
        self.assertEqual(ranking.rank({'runs': [run]})['status'], 'not_measured')

    def test_rank_is_disjoint_and_does_not_use_calls(self):
        run = self.fixture('baseline')[1][0]
        run['derived'] = {'journey_mean_ms': 10, 'time_ms_per_tick': {'timed_forward_and_cache_other': 5,
            'timed_reverse_cold': 1, 'schedule_query_inclusive': 0.8, 'database_tick_inclusive': 0.2, 'database_publication': 0.1},
            'calls': {'next_departures': 999999999}}
        result = ranking.rank({'runs': [run]})
        self.assertEqual(result['results'][0]['ranking'][0]['scope'], 'timed_forward_and_cache_other')
        self.assertAlmostEqual(sum(e['mean_ms_per_tick'] for e in result['results'][0]['ranking']), 10)
        self.assertNotIn('database_publication', [e['scope'] for e in result['results'][0]['ranking']])

    def test_archived_log_has_unknown_p50(self):
        path = pathlib.Path(__file__).parent.parent / "passenger-periodic-policy-residency-2026-10-13/compact-console.txt"
        parsed = summary.parse_console(path.read_text(encoding="utf-8"))
        result = summary.derived(parsed)
        self.assertEqual(result["completed_ticks"], 8192)
        self.assertIsNone(result["quantiles"]["simulation"]["p50"])
        self.assertAlmostEqual(result["simulation_mean_ms"], 33.772, places=2)
        self.assertEqual(result["calls"]["next_departures"], 179819090)
        self.assertIn("Work", parsed["periodic_outcomes"])
        self.assertIn("QueryWork", parsed["fallbacks"])

    def test_nearest_rank_and_overflow(self):
        buckets = [{"index": 0, "simulation": 50}, {"index": 2, "simulation": 49}, {"index": 2047, "simulation": 1}]
        self.assertEqual(summary.quantile(buckets, "simulation", 50, 3000000)["upper_us"], 1000)
        self.assertEqual(summary.quantile(buckets, "simulation", 99, 3000000)["upper_us"], 3000)
        self.assertIsNone(summary.quantile(buckets, "simulation", 100, 3000000)["upper_us"])
        self.assertTrue(summary.quantile(buckets, "simulation", 100, 3000000)["overflow"])
        self.assertEqual(summary.quantile([{"index": 0, "journey": 5}], "journey", 50, 0)["upper_us"], 0)

    def test_missing_measurements_stay_unknown(self):
        result = summary.derived(summary.parse_console("State ticks: 42\n"))
        self.assertIsNone(result["simulation_mean_ms"])
        self.assertIsNone(result["calls"]["timed_od"])

    def test_negative_numbers_and_transport(self):
        parsed = summary.parse_console("State ticks: -5\nState ticks: 8\nJourney transport: boarded=4; transferred=2; delivered=1 people\nFallback QueryWork: 7 packets; deferred=3 packets\n")
        self.assertEqual(parsed["state_ticks"], [-5, 8])
        self.assertEqual(parsed["fallbacks"]["QueryWork"]["deferred_packets"], 3)
        self.assertEqual(summary.semantic_signature(parsed)["transport_and_admission"]["Journey transport"]["delivered"], 1)

    def test_comparison_fingerprint(self):
        manifest = {"compiler": {"CMAKE_CXX_COMPILER_VERSION": "19.44", "CMAKE_PREFIX_PATH": "a"},
                    "package": {"input_sha256": "save", "asset_manifest_sha256": "asset", "config_sha256": "cfg"},
                    "host": {"cpu": "cpu"}, "settings": {"interval": 8}, "kind": "minimal", "harness_sha256": "h", "phase": "initial", "cache_off_scope": "OD"}
        run = {"diagnostics": {"state_ticks": [0, 8192]}}
        base = summary.fingerprint(manifest, run)
        manifest["compiler"]["CMAKE_PREFIX_PATH"] = "different-path-same-libs"
        self.assertEqual(base, summary.fingerprint(manifest, run))
        manifest["kind"] = "detail"
        self.assertNotEqual(base, summary.fingerprint(manifest, run))
        manifest["kind"] = "minimal"
        manifest["package"]["asset_manifest_sha256"] = "different-assets"
        self.assertNotEqual(base, summary.fingerprint(manifest, run))


if __name__ == "__main__":
    unittest.main()
