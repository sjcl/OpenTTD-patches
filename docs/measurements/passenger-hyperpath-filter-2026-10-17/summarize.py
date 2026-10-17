"""Validate OFF/ON/ON/OFF evidence and summarize actual simulation ticks."""
import argparse
import gzip
import json
import math
from pathlib import Path
import re


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_text(path):
    if path.is_file():
        return path.read_text(encoding='utf-8')
    return gzip.decompress(path.with_suffix(path.suffix + '.gz').read_bytes()).decode('utf-8')


def distribution(values):
    values = sorted(values)
    return dict(count=len(values), mean_us=sum(values) / len(values),
                p95_us=values[math.ceil(.95 * len(values)) - 1],
                p99_us=values[math.ceil(.99 * len(values)) - 1], max_us=values[-1])


def observation(console, prefix):
    lines = [line.split(prefix, 1)[1] for line in console.splitlines() if prefix in line]
    require(lines, f'Missing observation: {prefix}')
    return {k: (v == 'true' if v in ('true', 'false') else int(v))
            for k, v in re.findall(r'(\w+)=(-?\d+|true|false)', lines[-1])}


def compare(records, field):
    first = records[0][field]
    return {record['run']: {key: [first.get(key), record[field].get(key)]
                          for key in first.keys() | record[field].keys()
                          if first.get(key) != record[field].get(key)}
            for record in records[1:]}


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('runs', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    records = []
    for name, enabled in [('off-a', False), ('on-a', True), ('on-b', True), ('off-b', False)]:
        folder = args.runs / name
        process = json.loads((folder / 'process.json').read_text(encoding='utf-8'))
        console = read_text(folder / 'on-1-console.txt')
        script = (folder / 'scripts/game_start.scr').read_text(encoding='utf-8')
        switch = f'passenger_journey_stats hyperpath_filter_{"on" if enabled else "off"}\n'
        require(script.index(switch) < script.index('passenger_journey_stats reset\n') < script.index('unpause\n'), name + ': command order')
        require(process['hyperpath_filter'] == enabled and process['hyperpath'] is True, name + ': routing/filter mode')
        require(process['exit_code'] == 0 and not process['load_failed'] and process['warm_complete'], name + ': incomplete run')
        require(process['simulation_timing_valid'] and process['whole_process_timing_valid'] and not process['contamination'], name + ': overlap')
        require(not any(process[k] for k in ('trace_enabled', 'focused_od_trace', 'departure_diagnostic', 'validation_only', 'retirement_profile', 'retirement_reference')), name + ': unexpected observer')
        background = process['background_ready']
        require(background['completed'] and background['tick_before'] == background['tick_after'], name + ': background preparation')
        clock = process['actual-ticks.json']
        samples = clock['samples']
        ticks = process['requested_ticks']
        require(len(samples) == ticks and [s[0] for s in samples] == list(range(clock['start_tick'] + 1, clock['end_tick'] + 1)), name + ': tick window')
        midpoint = process['mid_rebuild_tick']
        require(midpoint == ticks // 2 and process['rebuild-transition.json']['elapsed_tick'] == midpoint, name + ': replacement tick')
        filters = observation(console, 'Passenger hyperpath boarding filter: ')
        require(filters['enabled'] == enabled, name + ': filter not applied')
        simulation = observation(console, 'Simulation tick time: ')
        require(simulation['completed_ticks'] == ticks and simulation['total_us'] == sum(s[1] for s in samples), name + ': diagnostic/sample disagreement')
        require(process['save-integrity.json']['payments_valid'], name + ': invalid payment')
        record = dict(run=name, filter=filters, input_sha256=process['input_sha256'],
                      binary_sha256=process['binary_sha256'], language_sha256=process['language_sha256'],
                      start_tick=clock['start_tick'], end_tick=clock['end_tick'], ticks=ticks,
                      midpoint=process['midpoint-save-integrity.json'], final=process['save-integrity.json'],
                      result_sha256=process['result_sha256'], simulation=simulation,
                      capacity=observation(console, 'Passenger capacity records: '),
                      transport={kind: observation(console, prefix + ' transport: ')
                                 for kind, prefix in [('journey', 'Journey'), ('schedule', 'Schedule'), ('untimed', 'Untimed')]},
                      inclusive=observation(console, 'Journey inclusive scopes: '),
                      process_cpu_seconds=process['process_cpu_seconds'], elapsed_seconds=process['elapsed_seconds'],
                      peak_working_bytes=process['peak_working_bytes'], peak_commit_bytes=process['peak_commit_bytes'],
                      newgrf_warning_lines=process['newgrf_warning_lines'],
                      script_error_pause_resumes=process['script_error_pause_resumes'])
        for phase, window in [('all', samples), ('ordinary', samples[:midpoint]), ('replacement', samples[midpoint:])]:
            record[phase] = {kind: distribution([s[column] for s in window])
                             for kind, column in [('simulation', 1), ('journey', 2)]}
        records.append(record)
    for key in ('input_sha256', 'binary_sha256', 'language_sha256', 'start_tick', 'end_tick', 'ticks'):
        require(len({r[key] for r in records}) == 1, 'Mixed ' + key)
    comparisons = {field: compare(records, field) for field in ('midpoint', 'final', 'transport')}
    require(not any(diff for field in comparisons.values() for diff in field.values()), 'Canonical or transport mismatch')
    off = records[0]['filter']
    for record in records:
        counts = record['filter']
        require(counts['groups_checked'] + counts['groups_skipped'] == off['groups_checked'], 'Group conservation')
        require(counts['groups_admitted'] == off['groups_admitted'], 'Admitted groups changed')
        require(counts['filtered_walks'] + counts['unfiltered_walks'] == off['unfiltered_walks'] > 0, 'Walk count changed')
        require(counts['groups_skipped'] > 0 if counts['enabled'] else counts['groups_skipped'] == counts['filtered_walks'] == 0, 'Inactive filter')
        require(all(record['capacity'][key] == records[0]['capacity'][key] for key in ('packets', 'passengers')), 'Capacity outcomes changed')
    pairs = []
    for before, after in [(records[0], records[1]), (records[3], records[2])]:
        pairs.append(dict(off=before['run'], on=after['run'], mean_reduction_percent={
            phase: {kind: 100 * (1 - after[phase][kind]['mean_us'] / before[phase][kind]['mean_us'])
                    for kind in ('simulation', 'journey')} for phase in ('all', 'ordinary', 'replacement')}))
    modes = {}
    for enabled in (False, True):
        windows = {'all': [], 'ordinary': [], 'replacement': []}
        for record in records:
            if record['filter']['enabled'] != enabled:
                continue
            samples = json.loads((args.runs / record['run'] / 'actual-ticks.json').read_text())['samples']
            half = len(samples) // 2
            for phase, window in [('all', samples), ('ordinary', samples[:half]), ('replacement', samples[half:])]:
                windows[phase].extend(window)
        modes['on' if enabled else 'off'] = {
            phase: {kind: distribution([s[column] for s in window])
                    for kind, column in [('simulation', 1), ('journey', 2)]}
            for phase, window in windows.items()}
    summary = dict(runs=records, canonical_comparisons=comparisons, pairs=pairs,
                   modes=modes,
                   saved_files_byte_identical=len({r['result_sha256'] for r in records}) == 1,
                   skipped_percent=100 * records[1]['filter']['groups_skipped'] / off['groups_checked'],
                   note='Per-tick times are elapsed simulation-thread times. Process CPU/peaks include load, paused builds and save. Two runs per mode are descriptive, not a confidence interval.')
    args.output.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
