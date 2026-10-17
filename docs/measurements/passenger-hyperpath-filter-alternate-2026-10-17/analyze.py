"""Validate one alternate-mode run and extrapolate its timed walk difference per tick."""
import argparse
import gzip
import json
import math
from pathlib import Path
import re


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    return path.read_text(encoding='utf-8') if path.exists() else gzip.decompress(
        path.with_suffix(path.suffix + '.gz').read_bytes()).decode('utf-8')


def observation(console, prefix):
    lines = [line.split(prefix, 1)[1] for line in console.splitlines() if prefix in line]
    require(lines, 'Missing diagnostic: ' + prefix)
    return {key: int(value) for key, value in re.findall(r'(\w+)=(-?\d+)', lines[-1])}, lines[-1]


def distribution(values):
    values = sorted(values)
    return dict(count=len(values), mean_us=sum(values) / len(values),
                p95_us=values[math.ceil(.95 * len(values)) - 1],
                p99_us=values[math.ceil(.99 * len(values)) - 1], max_us=values[-1])


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    reference_path = root / 'docs/measurements/passenger-hyperpath-filter-2026-10-17/summary.json'
    reference = json.loads(reference_path.read_text(encoding='utf-8'))['runs'][0]
    process = json.loads(read(args.run / 'process.json'))
    console = read(args.run / 'on-1-console.txt')
    script = read(args.run / 'scripts/game_start.scr')
    require(script.index('passenger_journey_stats hyperpath_filter_alternate\n') <
            script.index('passenger_journey_stats reset\n') < script.index('unpause\n'), 'Command order')
    require(process['hyperpath_filter_alternate'] and process['hyperpath_filter'] is None and
            process['hyperpath'] and process['precomputed'], 'Incorrect mode')
    require(process['exit_code'] == 0 and not process['load_failed'] and process['warm_complete'], 'Incomplete run')
    require(process['whole_process_timing_valid'] and process['simulation_timing_valid'] and
            not process['contamination'], 'Overlapping workload')
    require(not any(process[key] for key in ('trace_enabled', 'focused_od_trace', 'departure_diagnostic',
                'validation_only', 'retirement_profile', 'retirement_reference')), 'Unexpected observer')
    require(process['main_thread']['verified_mask'] == 1, 'Main thread affinity')
    background = process['background_ready']
    require(background['completed'] and background['tick_before'] == background['tick_after'], 'Background preparation')
    clock = process['actual-ticks.json']
    samples = clock['samples']
    ticks = process['requested_ticks']
    require(ticks == 2048 and len(samples) == ticks and
            [s[0] for s in samples] == list(range(clock['start_tick'] + 1, clock['end_tick'] + 1)), 'Tick window')
    require(clock['start_tick'] == reference['start_tick'] and clock['end_tick'] == reference['end_tick'] and
            process['input_sha256'] == reference['input_sha256'], 'Different input/window')
    require(process['mid_rebuild_tick'] == process['rebuild-transition.json']['elapsed_tick'] == 1024, 'Midpoint replacement')
    tick_diagnostic, _ = observation(console, 'Simulation tick time: ')
    require(tick_diagnostic['completed_ticks'] == ticks and
            tick_diagnostic['total_us'] == sum(s[1] for s in samples), 'Tick diagnostic mismatch')
    filters, mode_line = observation(console, 'Passenger hyperpath boarding filter: ')
    require('mode=alternate;' in mode_line, 'Console mode mismatch')
    timing, _ = observation(console, 'Passenger hyperpath walk time: ')
    walk_count = filters['filtered_walks'] + filters['unfiltered_walks']
    require(filters['filtered_walks'] > 0 and filters['unfiltered_walks'] > 0, 'Both modes must run')
    require(timing['filtered_groups'] + timing['unfiltered_groups'] ==
            filters['groups_checked'] + filters['groups_skipped'] == reference['filter']['groups_checked'], 'Group conservation')
    require(walk_count == reference['filter']['unfiltered_walks'] and
            filters['groups_admitted'] == reference['filter']['groups_admitted'], 'Walk/admission conservation')
    canonical = {}
    for key, expected in [('midpoint-save-integrity.json', reference['midpoint']), ('save-integrity.json', reference['final'])]:
        actual = process[key]
        differences = {name: [expected.get(name), actual.get(name)] for name in expected.keys() | actual.keys()
                       if expected.get(name) != actual.get(name)}
        require(not differences, key + ': ' + str(differences))
        canonical[key] = dict(compared_fields=len(expected), mismatches=differences)
    transport = {name: observation(console, prefix + ' transport: ')[0]
                 for name, prefix in [('journey', 'Journey'), ('schedule', 'Schedule'), ('untimed', 'Untimed')]}
    require(transport == reference['transport'], 'Transport mismatch')
    capacity, _ = observation(console, 'Passenger capacity records: ')
    require(all(capacity[key] == reference['capacity'][key] for key in ('packets', 'passengers')), 'Capacity outcomes changed')
    modes = {}
    for name in ('filtered', 'unfiltered'):
        count = filters[name + '_walks']
        modes[name] = dict(walks=count, groups=timing[name + '_groups'], microseconds=timing[name + '_us'],
                           us_per_walk=timing[name + '_us'] / count,
                           groups_per_walk=timing[name + '_groups'] / count)
    walks_per_tick = walk_count / ticks
    difference = modes['unfiltered']['us_per_walk'] - modes['filtered']['us_per_walk']
    saving_ms = difference * walks_per_tick / 1000
    result = dict(mode='alternate', input_sha256=process['input_sha256'], binary_sha256=process['binary_sha256'],
                  start_tick=clock['start_tick'], end_tick=clock['end_tick'], ticks=ticks,
                  filter=filters, walk_time=timing, modes=modes, walks_per_tick=walks_per_tick,
                  saving_us_per_walk=difference, saving_ms_per_tick_estimate=saving_ms,
                  saving_below_one_ms_per_tick=saving_ms < 1,
                  extrapolated_walk_ms_per_tick={name: row['us_per_walk'] * walks_per_tick / 1000
                                                  for name, row in modes.items()},
                  observed_mixed_walk_ms_per_tick=(timing['filtered_us'] + timing['unfiltered_us']) / ticks / 1000,
                  legacy_gap_reference_ms=5, saving_fraction_of_approximate_legacy_gap=saving_ms / 5,
                  canonical_comparisons=canonical, transport=transport, capacity=capacity,
                  final_save_sha256=process['result_sha256'],
                  same_save_bytes_as_previous_version=process['result_sha256'] == reference['result_sha256'],
                  newgrf_warning_lines=process['newgrf_warning_lines'],
                  script_error_pause_resumes=process['script_error_pause_resumes'],
                  process_cpu_seconds=process['process_cpu_seconds'], elapsed_seconds=process['elapsed_seconds'],
                  peak_working_bytes=process['peak_working_bytes'], peak_commit_bytes=process['peak_commit_bytes'],
                  reference='docs/measurements/passenger-hyperpath-filter-2026-10-17/summary.json',
                  note='One process; assignment is per loading context. Per-tick saving extrapolates the per-walk mean difference to all walks. The timer excludes preceding mode/mask setup and following key merge/candidate processing; it is not a complete tick profile.')
    for name, window in [('all', samples), ('ordinary', samples[:1024]), ('replacement', samples[1024:])]:
        result[name] = {kind: distribution([s[column] for s in window])
                        for kind, column in [('simulation', 1), ('journey', 2)]}
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
