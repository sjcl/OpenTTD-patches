"""Focused checks for evidence parsing, overflow handling and comparison gates."""
import importlib.util
import json
import pathlib
import tempfile
import unittest
from types import SimpleNamespace
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
runner_spec = importlib.util.spec_from_file_location("runner", pathlib.Path(__file__).with_name("Run-Profile.py"))
runner = importlib.util.module_from_spec(runner_spec)
runner_spec.loader.exec_module(runner)
archive_spec = importlib.util.spec_from_file_location('archive', pathlib.Path(__file__).with_name('Archive-Profile.py'))
archive = importlib.util.module_from_spec(archive_spec)
archive_spec.loader.exec_module(archive)


class EvidenceTests(unittest.TestCase):
    def test_archive_retains_raw_hashes_and_canonical_lf_evidence(self):
        with tempfile.TemporaryDirectory(dir=pathlib.Path(__file__).parent) as directory:
            root = pathlib.Path(directory)
            run_directory = root / 'on-1'
            run_directory.mkdir()
            console = b'State ticks: 1\r\nState ticks: 8193\r\n'
            (run_directory / 'on-1-console.txt').write_bytes(console)
            manifest = {'label': 'baseline', 'kind': 'minimal', 'phase': 'initial', 'revision': 'r',
                        'path': str(root / 'manifest.json'), 'runs': [{'id': 'on-1'}]}
            run = {'valid': True, 'label': 'baseline', 'kind': 'minimal', 'phase': 'initial', 'revision': 'r',
                   'id': 'on-1', 'directory': 'on-1', 'final_save_sha256': 'verified-save',
                   'process': {}, 'diagnostics': {}, 'evidence': {'console_sha256': archive.digest(console),
                   'stderr_sha256': 'stderr', 'stderr_bytes': 0, 'unique_stderr_lines': []}}
            source = root / 'summary.json'
            original = json.dumps({'manifests': [manifest], 'runs': [run]}, indent=2).replace('\n', '\r\n').encode('utf-8')
            source.write_bytes(original)
            output = root / 'archive'
            archive.archive(source, output)
            index = json.loads((output / 'collection-index.json').read_text(encoding='utf-8'))
            summary_bytes = (output / 'summary.json').read_bytes()
            self.assertNotIn(b'\r', summary_bytes)
            self.assertEqual(index['source_summary_sha256'], archive.digest(original))
            self.assertEqual(index['archived_summary_sha256'], archive.digest(summary_bytes))
            self.assertNotEqual(index['source_summary_sha256'], index['archived_summary_sha256'])
            archived_console = (output / index['runs'][0]['archive_directory'] / 'console.txt').read_bytes()
            self.assertEqual(archived_console, console.replace(b'\r\n', b'\n'))
            self.assertEqual(index['runs'][0]['raw_console_sha256'], archive.digest(console))
            self.assertEqual(index['runs'][0]['archived_console_sha256'], archive.digest(archived_console))

    def test_busy_lock_prevents_package_reads_and_manifest_creation(self):
        args = SimpleNamespace(wait_idle_seconds=0, output=pathlib.Path('output'))
        with patch.object(runner, 'acquire_measurement_lock', side_effect=RuntimeError('busy')), \
             patch.object(runner, 'run_locked') as prepare, \
             patch.object(pathlib.Path, 'unlink') as remove:
            with self.assertRaisesRegex(RuntimeError, 'busy'):
                runner.run(args)
            prepare.assert_not_called()
            remove.assert_not_called()

    def test_measurement_lock_waits_for_owner_release(self):
        clock = [0]
        def advance(seconds):
            clock[0] += seconds
        with patch.object(runner.os, 'open', side_effect=[FileExistsError(), 42]), \
             patch.object(runner.time, 'monotonic', side_effect=lambda: clock[0]), \
             patch.object(runner.time, 'sleep', side_effect=advance), patch('builtins.print'):
            self.assertEqual(runner.acquire_measurement_lock(pathlib.Path('lock'), 20), 42)
        self.assertEqual(clock[0], 5)

    def test_measurement_lock_timeout_does_not_remove_owner_lock_or_stop_owner(self):
        clock = [0]
        def advance(seconds):
            clock[0] += seconds
        with patch.object(runner.os, 'open', side_effect=FileExistsError()), \
             patch.object(runner.time, 'monotonic', side_effect=lambda: clock[0]), \
             patch.object(runner.time, 'sleep', side_effect=advance), \
             patch.object(pathlib.Path, 'unlink') as remove, \
             patch.object(runner, 'abort_own_game') as abort, patch('builtins.print'):
            with self.assertRaisesRegex(RuntimeError, 'existing execution was not stopped'):
                runner.acquire_measurement_lock(pathlib.Path('lock'), 20)
            remove.assert_not_called()
            abort.assert_not_called()
        self.assertEqual(clock[0], 20)

    def test_idle_grace_restarts_after_competing_execution(self):
        clock = [0]
        def advance(seconds):
            clock[0] += seconds
        with patch.object(runner, 'competing_processes', side_effect=[['cmake:1'], [], ['ctest:2'], [], [], []]), \
             patch.object(runner.time, 'monotonic', side_effect=lambda: clock[0]), \
             patch.object(runner.time, 'sleep', side_effect=advance), patch('builtins.print'):
            runner.wait_for_idle(120, 20)
        self.assertEqual(clock[0], 50)

    def test_idle_wait_times_out_without_stopping_another_process(self):
        clock = [0]
        def advance(seconds):
            clock[0] += seconds
        with patch.object(runner, 'competing_processes', return_value=['ctest:2']), \
             patch.object(runner.time, 'monotonic', side_effect=lambda: clock[0]), \
             patch.object(runner.time, 'sleep', side_effect=advance), \
             patch.object(runner, 'abort_own_game') as abort, patch('builtins.print'):
            with self.assertRaisesRegex(RuntimeError, 'did not remain idle'):
                runner.wait_for_idle(20)
            abort.assert_not_called()

    def test_observed_calls_keep_short_circuit_and_nested_arguments(self):
        expression = 'ok && Station::GetIfValid((*graph)[node].Station()) != nullptr'
        result = instrument.observe_calls(expression, 'Station::GetIfValid', 'live.station_get_if_valid')
        self.assertEqual(result, 'ok && JourneyProfile::Observe<"live.station_get_if_valid">(Station::GetIfValid((*graph)[node].Station())) != nullptr')
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

    def test_comparison_retains_noise_and_separates_save_from_logical_semantics(self):
        fixtures = {"baseline": self.fixture('baseline'), "A": self.fixture('A', duration=27)}
        second = self.fixture('A', duration=35)[1][0]
        second['id'] = 'on-2'
        fixtures['A'][1].append(second)
        for run in fixtures['A'][1]:
            run['final_save_sha256'] = 'another-save'
        with patch.object(summary, 'read_manifest', side_effect=lambda p: fixtures[p.name]):
            result = summary.summarize([pathlib.Path(p) for p in fixtures], 'baseline')
        comparison = result['comparisons'][0]
        self.assertTrue(comparison['compatible'])
        self.assertEqual(comparison['run_mean_relative_ranges']['baseline'], 0)
        self.assertAlmostEqual(comparison['run_mean_relative_ranges']['candidate'], 8 / 31)
        self.assertTrue(comparison['semantics_match_baseline'])
        self.assertFalse(comparison['full_save_match_baseline'])

    def test_archived_baseline_can_compare_new_run_without_old_save_files(self):
        baseline = self.fixture('baseline')
        baseline[0].update(revision='old', runs=[{'id': 'on-1'}])
        baseline[1][0]['revision'] = 'old'
        with patch.object(summary, 'read_manifest', return_value=baseline):
            archived = summary.summarize([pathlib.Path('missing-baseline-manifest')], 'baseline')
        with tempfile.TemporaryDirectory(dir=pathlib.Path(__file__).parent) as directory:
            path = pathlib.Path(directory) / 'summary.json'
            path.write_text(json.dumps(archived), encoding='utf-8')
            with patch.object(summary, 'read_manifest', return_value=self.fixture('A', duration=27)) as read:
                result = summary.summarize([pathlib.Path('new-manifest')], 'baseline', [path])
            read.assert_called_once_with(pathlib.Path('new-manifest'))
            self.assertEqual(len(result['runs']), 2)
            self.assertTrue(result['comparisons'][0]['compatible'])
            self.assertAlmostEqual(result['comparisons'][0]['relative_change'], 27 / 35 - 1)
            archived['runs'][0]['revision'] = 'missing-provenance'
            path.write_text(json.dumps(archived), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'lacks matching manifest'):
                list(summary.snapshot_datasets(path))

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

    def test_logical_work_and_physical_departure_calls_are_separate(self):
        parsed = summary.parse_console('Simulation tick time: completed_ticks=2; total_us=2000; max_us=1200\n'
            'Periodic database: searches=3; next_departures=100\n'
            'Periodic departures: logical_vehicles=100; physical_calculations=4; search_work=125; forward_us=600\n')
        result = summary.derived(parsed)
        self.assertEqual(result['calls']['next_departures'], 100)
        self.assertEqual(result['calls']['physical_departure_calculations'], 4)
        self.assertEqual(result['time_ms_per_tick']['timed_forward'], 0.3)
        self.assertEqual(summary.semantic_signature(parsed)['timed_search_work'], 125)
        parsed['records']['Periodic departures']['physical_calculations'] = 99
        self.assertEqual(summary.semantic_signature(parsed)['timed_search_work'], 125)

    def test_physical_fallback_bfs_count_does_not_change_semantics(self):
        a = summary.parse_console('OD fallback: work=99; path_searches=3\n')
        b = summary.parse_console('OD fallback: work=99; path_searches=7\n')
        self.assertEqual(summary.semantic_signature(a), summary.semantic_signature(b))
        b['records']['OD fallback']['work'] = 100
        self.assertNotEqual(summary.semantic_signature(a), summary.semantic_signature(b))

    def test_detail_windows_use_deltas_and_exact_sample_quantiles(self):
        detail = {'tick_samples': [[10, 2], [30, 6], [20, 4], [40, 8]], 'checkpoints': [
            {'completed_ticks': 2, 'metrics': {'query': {'calls': 3, 'inclusive_ns': 1000, 'exclusive_ns': 400}}},
            {'completed_ticks': 4, 'metrics': {'query': {'calls': 8, 'inclusive_ns': 4000, 'exclusive_ns': 1400}}}]}
        windows = summary.detail_windows(detail)
        self.assertEqual(windows[1]['first_ordinal'], 3)
        self.assertEqual(windows[1]['metrics']['query']['calls'], 5)
        self.assertEqual(windows[1]['metrics']['query']['inclusive_ns'], 3000)
        self.assertEqual(windows[1]['metrics']['query']['exclusive_ns'], 1000)
        self.assertEqual(windows[1]['simulation']['p50_us'], 20)
        self.assertEqual(windows[1]['simulation']['p99_us'], 40)
        self.assertEqual(windows[1]['journey']['mean_us'], 6)

    def test_detail_windows_reject_decreased_counters_and_invalid_intervals(self):
        detail = {'tick_samples': [[10, 2], [20, 4]], 'checkpoints': [
            {'completed_ticks': 1, 'metrics': {'query': {'calls': 3}}},
            {'completed_ticks': 2, 'metrics': {'query': {'calls': 2}}}]}
        with self.assertRaisesRegex(ValueError, 'counter decreased'):
            summary.detail_windows(detail)
        detail['checkpoints'] = [{'completed_ticks': 3, 'metrics': {}}]
        with self.assertRaisesRegex(ValueError, 'outside recorded'):
            summary.detail_windows(detail)

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
