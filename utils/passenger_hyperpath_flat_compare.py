#!/usr/bin/env python3
"""Compare complete real-map publication witnesses in separate measured processes."""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

import passenger_precomputed_real_map as real


def run(args, flat, name=None, replay_work=None):
    directory = args.output / (name or ('flat' if flat else 'previous'))
    directory.mkdir()
    witness = directory / 'policies.bin'
    env = dict(os.environ, OPENTTD_TABLE_DATABASE=str((args.fixture / 'database.bin').resolve()),
               OPENTTD_TABLE_STATIONS=str((args.fixture / 'stations.txt').resolve()),
               OPENTTD_TABLE_WITNESS=str(witness.resolve()), OPENTTD_TABLE_FLAT=str(int(flat)))
    env.pop('OPENTTD_TABLE_REPLAY_WORK', None)
    if replay_work is not None:
        env['OPENTTD_TABLE_REPLAY_WORK'] = str(replay_work)
    real.PROFILE.wait_for_idle(args.measurement_wait, 5)
    command = [str(args.binary.resolve()), '[.hyperpath-flat-real]', '-r', 'compact']
    started, observed, contamination, last_census = time.monotonic(), {}, [], 0
    with (directory / 'raw.log').open('wb') as stream:
        process = subprocess.Popen(command, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            while process.poll() is None:
                for key, value in real.CPU.observe(process).items():
                    observed[key] = max(observed.get(key, 0), value)
                if time.monotonic() - last_census >= 1:
                    own = args.binary.stem.lower() + ':' + str(process.pid)
                    busy = [item for item in real.PROFILE.competing_processes() if item.lower() != own]
                    if busy:
                        contamination.append(busy)
                        raise RuntimeError('Competing measurement detected; stop only this benchmark')
                    last_census = time.monotonic()
                if time.monotonic() - started >= args.timeout:
                    raise TimeoutError('Own flat-policy benchmark timed out')
                time.sleep(.1)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            for key, value in real.CPU.observe(process).items():
                observed[key] = max(observed.get(key, 0), value)
    counters, targets = {}, {}
    fields = dict(flat_build='flat rows words operations envelope bytes elapsed_ms'.split(),
                  flat_memory='reported graph labels indices policy_headers candidate_arrays local differences additions maximum_additions multi_addition_differences unexpected_removals'.split())
    for row in csv.reader((directory / 'raw.log').read_text(encoding='utf-8', errors='replace').splitlines()):
        if row and row[0] in fields:
            if len(row) != len(fields[row[0]]) + 1:
                raise ValueError(f'Malformed counter row: {row}')
            counters[row[0]] = dict(zip(fields[row[0]], map(int, row[1:])))
        elif row and row[0] == 'flat_target':
            origin, destination, cost, wait, count = map(int, row[1:])
            targets[f'{origin}:{destination}'] = dict(cost=cost, wait=wait, candidate_count=count, candidates=[])
        elif row and row[0] == 'flat_target_candidate':
            origin, destination, service, order, occurrence = map(int, row[1:])
            targets[f'{origin}:{destination}']['candidates'].append(dict(service=service, order=order, occurrence=occurrence))
    result = dict(command=command, exit_code=process.returncode, elapsed_seconds=time.monotonic() - started,
                  contamination=contamination, binary_sha256=real.PROFILE.sha256(args.binary),
                  database_sha256=real.PROFILE.sha256(args.fixture / 'database.bin'),
                  stations_sha256=real.PROFILE.sha256(args.fixture / 'stations.txt'), counters=counters, target_policies=targets, **observed)
    if witness.is_file():
        result.update(witness_bytes=witness.stat().st_size, witness_sha256=real.PROFILE.sha256(witness))
    real.dump(directory / 'result.json', result)
    if process.returncode or contamination or set(counters) != set(fields):
        raise RuntimeError(f'Invalid benchmark; see {directory}')
    return result


def compare(first, second):
    offset = 0
    with first.open('rb') as a, second.open('rb') as b:
        while True:
            left, right = a.read(16 * 1024 * 1024), b.read(16 * 1024 * 1024)
            if left != right:
                differing = next((i for i, values in enumerate(zip(left, right)) if values[0] != values[1]), min(len(left), len(right)))
                return dict(equal=False, first_difference_byte=offset + differing)
            offset += len(left)
            if not left:
                return dict(equal=True, compared_bytes=offset)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('fixture', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--measurement-wait', type=int, default=1800)
    parser.add_argument('--timeout', type=int, default=7200)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    # Three full normalized witnesses contain every state, candidate, label and
    # exact fixed-point numerator/frequency. Keep generous room before launch.
    if shutil.disk_usage(args.output).free < 40 * 1024**3:
        raise RuntimeError('Complete witnesses require at least 40 GiB of free output space')
    lease = Path(tempfile.gettempdir()) / 'openttd-passenger-routing-measurement.lock'
    fd = real.PROFILE.acquire_measurement_lock(lease, args.measurement_wait)
    try:
        os.write(fd, json.dumps(dict(pid=os.getpid(), output=str(args.output.resolve()), purpose='Exact flat-policy and N/F comparison')).encode())
        previous, flat = run(args, False), run(args, True)
        real.dump(args.output / 'saved-work.json', dict(operations=flat['counters']['flat_build']['operations'],
            database_sha256=flat['database_sha256'], stations_sha256=flat['stations_sha256'], flat=True,
            revisit_dominance=True, continuation_witness=True, transfer=20))
        saved_work = json.loads((args.output / 'saved-work.json').read_text(encoding='utf-8'))
        replay = run(args, True, name='replay', replay_work=saved_work['operations'])
        exact = compare(args.output / 'previous/policies.bin', args.output / 'flat/policies.bin')
        exact_replay = compare(args.output / 'flat/policies.bin', args.output / 'replay/policies.bin')
        operations_equal = previous['counters']['flat_build']['operations'] == flat['counters']['flat_build']['operations']
        targets_equal = previous['target_policies'] == flat['target_policies'] == replay['target_policies']
        real.dump(args.output / 'comparison.json', dict(previous=previous, flat=flat, replay=replay, exact=exact, exact_replay=exact_replay, operations_equal=operations_equal,
            targets_equal=targets_equal,
            note='Separate processes include the identical frozen map/DB and one table each. Replay uses the persisted Work count with prime-size slices. OS peaks are process-wide; component counters describe completed table residency. Native JPSD and MP tests separately cover wire identities and mixed generations.'))
        if not exact['equal'] or not exact_replay['equal'] or not operations_equal or not targets_equal:
            raise RuntimeError('Flat-policy witness or logical Work differs')
    finally:
        os.close(fd)
        lease.unlink()


if __name__ == '__main__':
    main()
