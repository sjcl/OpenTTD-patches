"""Read active event gauges from end saves before the dedicated server's replay.

Requires the copied inspector/stdio TUs from passenger_precomputed_join_prepare.py.
The startup script quits without advancing simulation. Canonical state is checked
against each run's end snapshot; derived ready-row counters await replay and are
excluded from that comparison. This is not a performance measurement.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess

import passenger_precomputed_real_map as real


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'package', 'lang', 'output'):
        parser.add_argument(name, type=pathlib.Path)
    parser.add_argument('runs', type=pathlib.Path, nargs='+')
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
        (scripts / 'game_start.scr').write_text(f'script {log}\ngetfulldate\npassenger_journey_stats total\n'
                                              'passenger_journey_stats save_integrity\nscript\nquit\n', encoding='utf-8')
        command = [str(args.binary.resolve()), '-x', '-X', '-c', 'openttd.cfg', '-snull', '-mnull',
                   f'-D127.0.0.1:{17610 + index}', '-g', 'input.sav']
        subprocess.run(command, cwd=directory, env=dict(os.environ, OPENTTD_PASSENGER_MEASURE='1'),
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       creationflags=subprocess.CREATE_NO_WINDOW, timeout=300, check=True)
        expected = json.loads((run / 'save-integrity.json').read_text(encoding='utf-8'))
        actual = json.loads((directory / 'save-integrity.json').read_text(encoding='utf-8'))
        derived = {'completed_rows', 'retained_rows', 'table_operations'}
        mismatches = {key:[expected.get(key), actual.get(key)] for key in expected.keys() | actual.keys()
                      if key not in derived and expected.get(key) != actual.get(key)}
        if mismatches:
            raise RuntimeError(f'Readback changed canonical state: {run.name}: {mismatches}')
        console = (directory / 'console.txt').read_text(encoding='utf-8')
        line = next(line for line in console.splitlines() if line.startswith('Passenger events: '))
        events = {key:int(value) for key, value in re.findall(r'(\w+)=(-?\d+)', line)}
        results.append(dict(run=run.name, canonical_mismatches=mismatches, state_ticks=actual['state_ticks'],
                            active_events=events))
        print(run.name, events, flush=True)
    (output / 'result.json').write_text(json.dumps(dict(runs=results), indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
