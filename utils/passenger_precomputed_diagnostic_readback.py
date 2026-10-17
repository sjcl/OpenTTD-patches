"""Read active event gauges from end saves before the dedicated server's replay.

Requires the copied inspector/stdio TUs from passenger_precomputed_join_prepare.py.
The startup script quits without advancing simulation. Canonical state is checked
against each run's end snapshot; derived ready-row counters await replay and are
excluded from that comparison. This is not a performance measurement.
"""
import argparse
import json
import lzma
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

import passenger_precomputed_real_map as real


def verify_wire_upgrade(saved_wire, loaded_wire):
    """Only append neutral algorithm identity when loading an older JPSD."""
    version = int.from_bytes(saved_wire[:8], 'little')
    target = int.from_bytes(loaded_wire[:8], 'little')
    if version not in (12, 13, 14, 15) or target not in (14, 15) or target < version:
        raise ValueError('Memory readback expects JPSD v12 through v15')
    expected = loaded_wire
    if target >= 15 and version < 15:
        if len(expected) < 24 or any(expected[-16:]):
            raise RuntimeError('An old snapshot acquired flat-storage identity')
        expected = (14).to_bytes(8, 'little') + expected[8:-16]
    if version < 14:
        words = int.from_bytes(expected[-8:], 'little')
        if not 2 <= words < len(expected) // 8 or any(expected[-(words + 1) * 8:-8]):
            raise RuntimeError('An old snapshot acquired unrecorded witness/facility state')
        prefix = (13).to_bytes(8, 'little') + expected[8:-(words + 1) * 8]
        original = (13).to_bytes(8, 'little') + saved_wire[8:] + bytes(16) if version == 12 else saved_wire
        verified = prefix == original
    else:
        verified = expected == saved_wire
    if not verified:
        raise RuntimeError('The loader changed saved algorithm state beyond the verified old-prefix wire upgrade')
    return dict(from_version=version, to_version=target, bytes_verified=True, replay_identical=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'package', 'lang', 'output'):
        parser.add_argument(name, type=pathlib.Path)
    parser.add_argument('runs', type=pathlib.Path, nargs='+')
    parser.add_argument('--hyperpath-census', action='store_true', help='Read the failure-state merge counterfactual from a Hyperpath inspector')
    parser.add_argument('--replay-memory', action='store_true', help='Replay only the saved Work, then inspect current/retained allocation components')
    args = parser.parse_args()
    output = args.output.resolve()
    if any(character.isspace() for character in str(output)):
        raise ValueError('Console script output paths must not contain whitespace')
    output.mkdir(parents=True, exist_ok=False)
    results = []
    for index, run in enumerate(args.runs):
        run = run.resolve()
        directory = output / run.name
        directory.mkdir()
        for name in real.PROFILE.ASSET_DIRS:
            if name != 'lang' and (args.package / name).is_dir():
                shutil.copytree(args.package / name, directory / name, copy_function=real.PROFILE.hardlink_or_copy)
        shutil.copytree(args.lang, directory / 'lang', copy_function=real.PROFILE.hardlink_or_copy)
        shutil.copy2(run / 'openttd.cfg', directory / 'openttd.cfg')
        with (directory / 'openttd.cfg').open('a', encoding='utf-8') as config:
            config.write('\n[network]\nserver_game_type = local\n')
        shutil.copy2(run / 'result.sav', directory / 'input.sav')
        scripts = directory / 'scripts'
        scripts.mkdir()
        log = (directory / 'console.txt').as_posix()
        census = 'passenger_journey_stats hyperpath_merge_census\n' if args.hyperpath_census else ''
        if args.replay_memory:
            census += 'passenger_journey_stats replay_precomputed\npassenger_journey_stats hyperpath_memory\n'
        (scripts / 'game_start.scr').write_text(f'script {log}\ngetfulldate\npassenger_journey_stats total\npassenger_journey_stats inventory\n' + census +
                                              'passenger_journey_stats save_integrity\nscript\nquit\n', encoding='utf-8')
        command = [str(args.binary.resolve()), '-x', '-X', '-c', 'openttd.cfg', '-snull', '-mnull',
                   f'-D127.0.0.1:{17610 + index}', '-g', 'input.sav']
        subprocess.run(command, cwd=directory, env=dict(os.environ, OPENTTD_PASSENGER_MEASURE='1'),
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       creationflags=subprocess.CREATE_NO_WINDOW, timeout=900 if args.replay_memory else 300, check=True)
        expected = json.loads((run / 'save-integrity.json').read_text(encoding='utf-8'))
        actual = json.loads((directory / 'save-integrity.json').read_text(encoding='utf-8'))
        derived = {'completed_rows', 'retained_rows', 'table_operations'}
        wire_upgrade = None
        if args.replay_memory:
            console = (directory / 'console.txt').read_text(encoding='utf-8')
            if 'Hyperpath replay: ready=true; saved_state_identical=true;' not in console:
                raise RuntimeError('Saved Work replay did not confirm identical database bytes and StateTicks')
            raw = (run / 'result.sav').read_bytes()
            if raw[:4] != b'OTTX':
                raise ValueError('Memory readback requires the fixture XZ save layout')
            decoded = lzma.decompress(raw[8:])
            at = decoded.rindex(b'JPSD')
            riff = int.from_bytes(decoded[at + 4:at + 8], 'big')
            if riff & 0x0F000000:
                raise ValueError('Unexpected JPSD chunk type')
            size = (riff & 0xFFFFFF) | ((riff >> 28) << 24)
            saved_wire = decoded[at + 8:at + 8 + size]
            if len(saved_wire) != size:
                raise ValueError('Truncated saved JPSD')
            expected_wire = (directory / 'hyperpath-replay-before.bin').read_bytes()
            wire_upgrade = verify_wire_upgrade(saved_wire, expected_wire)
            derived.add('database_digest')
            del raw, decoded, saved_wire, expected_wire
        mismatches = {key:[expected.get(key), actual.get(key)] for key in expected.keys() | actual.keys()
                      if key not in derived and expected.get(key) != actual.get(key)}
        if mismatches:
            raise RuntimeError(f'Readback changed canonical state: {run.name}: {mismatches}')
        console = (directory / 'console.txt').read_text(encoding='utf-8')
        line = next(line for line in console.splitlines() if 'Passenger events: ' in line)
        events = {key:int(value) for key, value in re.findall(r'(\w+)=(-?\d+)', line)}
        results.append(dict(run=run.name, canonical_mismatches=mismatches, state_ticks=actual['state_ticks'],
                            active_events=events))
        if args.replay_memory:
            results[-1]['database_wire'] = wire_upgrade
            results[-1]['hyperpath_memory'] = [{key:int(value) for key, value in re.findall(r'(\w+)=(-?\d+)', line)} |
                {'retained': 'retained=true' in line} for line in console.splitlines() if 'Hyperpath memory: ' in line]
        inventory_line = next(line for line in console.splitlines() if 'Passenger inventory: ' in line)
        if 'Passenger inventory: complete;' not in inventory_line:
            raise RuntimeError(f'The endpoint passenger inventory is incomplete: {run.name}')
        inventory = {key:int(value) for key, value in re.findall(r'(\w+)=(-?\d+)', inventory_line)}
        modes = {}
        for line in console.splitlines():
            match = re.search(r'Inventory (Legacy|Journey|Fallback): (\d+) people/(\d+) packets; known_OD=(\d+) people', line)
            if match:
                modes[match[1]] = dict(people=int(match[2]), packets=int(match[3]), known_OD=int(match[4]))
        if len(modes) != 3 or inventory['available'] + inventory['reserved'] != inventory['waiting'] or inventory['waiting'] > sum(mode['people'] for mode in modes.values()):
            raise RuntimeError(f'Inconsistent endpoint inventory: {run.name}')
        metadata = run / 'transport-trace-meta.txt'
        if metadata.is_file():
            counts = {key:int(value) for key, value in re.findall(r'^(\w+)=(\d+)$', metadata.read_text(encoding='utf-8'), re.M)}
            if sum(mode['people'] for mode in modes.values()) != counts['remaining_passengers'] or sum(mode['known_OD'] for mode in modes.values()) != counts['remaining_known_passengers']:
                raise RuntimeError(f'End-save inventory differs from the live transport census: {run.name}')
        results[-1]['passenger_inventory'] = dict(complete=True, **inventory, modes=modes,
            total_people=sum(mode['people'] for mode in modes.values()),
            physically_onboard_people=sum(mode['people'] for mode in modes.values()) - inventory['waiting'])
        if args.hyperpath_census:
            results[-1]['merge_census'] = json.loads((directory / 'hyperpath-merge-census.json').read_text(encoding='utf-8'))
        print(run.name, events, flush=True)
    (output / 'result.json').write_text(json.dumps(dict(runs=results), indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    if '-h' in sys.argv or '--help' in sys.argv:
        main()
    else:
        lock_path = pathlib.Path(tempfile.gettempdir()) / 'openttd-passenger-routing-measurement.lock'
        try:
            parent_owns_lease = json.loads(lock_path.read_text(encoding='utf-8')).get('pid') == os.getppid()
        except (FileNotFoundError, json.JSONDecodeError):
            parent_owns_lease = False
        lock = None
        try:
            if not parent_owns_lease:
                lock = real.PROFILE.acquire_measurement_lock(lock_path, 14400)
                os.write(lock, json.dumps(dict(pid=os.getpid(), purpose='canonical end-save readback', output=sys.argv[4])).encode())
                real.PROFILE.wait_for_idle(1800, 5)
            main()
        finally:
            if lock is not None:
                os.close(lock)
                lock_path.unlink()
