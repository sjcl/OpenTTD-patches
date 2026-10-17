"""Complete-table game peak memory: fixed cold save, dense/new/new/dense."""
import argparse
import hashlib
import json
import os
import pathlib
import time

import passenger_precomputed_real_map as real


def main():
    parser = argparse.ArgumentParser(__doc__)
    for name in ('binary', 'package', 'input', 'lang', 'output'):
        parser.add_argument(name, type=pathlib.Path)
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--measurement-wait', type=int, default=14400)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binary_sha = hashlib.sha256(args.binary.read_bytes()).hexdigest()
    previous = os.environ.get('OPENTTD_PRECOMPUTED_DENSE_ROWS')

    def run_case(name, input_path, warm, dense):
        if dense:
            os.environ['OPENTTD_PRECOMPUTED_DENSE_ROWS'] = '1'
        else:
            os.environ.pop('OPENTTD_PRECOMPUTED_DENSE_ROWS', None)
        folder = args.output / name
        record = folder / 'process.json'
        input_sha = hashlib.sha256(input_path.read_bytes()).hexdigest()
        reused = args.resume and record.is_file()
        if not reused:
            if args.resume and folder.exists():
                assert folder.resolve().parent == args.output.resolve() and not any(folder.iterdir())
                folder.rmdir()
            options = argparse.Namespace(binary=args.binary, package=args.package, input=input_path, lang=args.lang, output=folder,
                ticks=0, timeout=1800, resume=True, quality_probe=False, fixture_timetable=False, fixture_estimated=False,
                cold_precomputed=False, warm_precomputed=warm, rebuild_precomputed=False, precomputed=warm,
                unpause_before_save=False, validation_only=False, main_thread_affinity_mask=1, wait_background_ready=warm,
                observe_host=False, measurement_wait=args.measurement_wait)
            real.run(options)
        process = json.loads(record.read_text(encoding='utf-8'))
        assert process['exit_code'] == 0 and not process.get('load_failed') and process['whole_process_timing_valid']
        assert process['binary_sha256'] == binary_sha and process['input_sha256'] == input_sha
        if warm:
            assert process['warm_complete']
            stderr = (folder / 'stderr.txt').read_text(encoding='utf-8', errors='replace')
            assert f'Precomputed row storage control: dense={int(dense)}' in stderr
            if reused:
                assert process['row_dense_control'] == dense
            else:
                process['row_dense_control'] = dense
                process['row_mode_verified_in_child_log'] = True
                record.write_text(json.dumps(process, indent=2) + '\n', encoding='utf-8')
        state = json.loads((folder / 'save-integrity.json').read_text(encoding='utf-8'))
        assert process['requested_ticks'] == 0 and not process['contamination']
        clock_file = folder / 'actual-ticks.json'
        if clock_file.is_file():
            clock = json.loads(clock_file.read_text(encoding='utf-8'))
            assert clock['start_tick'] == clock['end_tick'] == state['state_ticks'] and not clock['samples']
        if not reused:
            time.sleep(10)
        return folder, process, state

    try:
        cold, _, initial = run_case('cold-setup', args.input, False, False)
        assert initial['precomputed_routing'] == 0 and initial['completed_rows'] == 0 and initial['retained_rows'] == 0 and initial['table_operations'] == 0
        cold_input = cold / 'result.sav'
        assert cold_input.is_file()
        canonical = None
        runs = []
        for name, dense in [('normal-old', True), ('normal-new', False), ('reverse-new', False), ('reverse-old', True)]:
            folder, process, state = run_case(name, cold_input, True, dense)
            assert state['precomputed_routing'] == 1 and state['completed_rows'] > 0 and state['retained_rows'] == 0
            assert state['state_ticks'] == initial['state_ticks']
            if canonical is None: canonical = state
            assert state == canonical, (name, state, canonical)
            runs.append(dict(name=name, canonical_equal=True, dense_control=dense,
                completed_rows=state['completed_rows'], table_operations=state['table_operations'],
                peak_working_bytes=process['peak_working_bytes'], peak_commit_bytes=process['peak_commit_bytes']))
            (args.output / 'comparison-summary.json').write_text(json.dumps(dict(binary_sha256=binary_sha,
                cold_input_sha256=hashlib.sha256(cold_input.read_bytes()).hexdigest(), canonical=canonical, runs=runs), indent=2) + '\n', encoding='utf-8')
    finally:
        if previous is None: os.environ.pop('OPENTTD_PRECOMPUTED_DENSE_ROWS', None)
        else: os.environ['OPENTTD_PRECOMPUTED_DENSE_ROWS'] = previous


if __name__ == '__main__':
    main()
