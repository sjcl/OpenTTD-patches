"""Audit build/test/game process lifetimes against the actual WPR marker interval."""
import argparse
import json
import re
import subprocess
from pathlib import Path

PROCESS = re.compile(r'^\s*(MIN|\d+),\s*(MAX|\d+),\s*Process,\s*[^,]+,\s*(.+?)\s+\(\s*(\d+)\),')
BUSY = re.compile(r'^(openttd.*|regression_.*|passenger_.*|cmake|MSBuild|ninja|cl|ctest|vstest.*|clang.*|gcc|g\+\+)\.exe$', re.I)
MARK = re.compile(r'^\s*Mark,\s*(\d+),\s*(.*?)\s*$')


def analyze(process_text, marker_text, mode, own_pid):
    marks = {match[2]:int(match[1]) for line in marker_text.splitlines() if (match := MARK.match(line))}
    begin, end = marks[mode+'-begin'], marks[mode+'-end']
    assert begin < end
    relevant, overlapping = [], []
    found_game = False
    for line in process_text.splitlines():
        match = PROCESS.match(line)
        if not match:
            continue
        name, pid = match[3].strip(), int(match[4])
        if not BUSY.fullmatch(name):
            continue
        start = None if match[1] == 'MIN' else int(match[1])
        stop = None if match[2] == 'MAX' else int(match[2])
        own = pid == own_pid and name.lower() == 'openttd.exe'
        overlaps = (start is None or start < end) and (stop is None or stop > begin)
        row = dict(name=name, pid=pid, start_us=start, end_us=stop, own_game=own, overlaps_window=overlaps)
        relevant.append(row)
        if own and (start is None or start <= begin) and (stop is None or stop >= end):
            found_game = True
        elif overlaps:
            overlapping.append(row)
    assert found_game, 'The measured game is missing from the ETL process table'
    return dict(mode=mode, marker_begin_us=begin, marker_end_us=end, game_pid=own_pid,
        valid=not overlapping, overlapping_competing_processes=overlapping, relevant_processes=relevant,
        method='Kernel process lifetimes in ETL; MIN/MAX denote trace boundaries. Match the runner build/test/game names, excluding only the measured openttd.exe PID. Units are microseconds.')


def audit_window(directory, toolkit, mode):
    process_file, marker_file = directory/'window-processes.txt', directory/'window-markers.txt'
    command = [str(toolkit/'xperf.exe'), '-i', str(directory/'cpu.etl'), '-target', 'machine',
        '-a', 'process', '-ao', str(process_file), '-a', 'marks', '-summary', '-ao', str(marker_file)]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    own_pid = int((directory/'wpr-ready.txt').read_text().split()[0])
    result = analyze(process_file.read_text(encoding='utf-8-sig', errors='replace'),
        marker_file.read_text(encoding='utf-8-sig', errors='replace'), mode, own_pid)
    (directory/'window-audit.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    if not result['valid']:
        raise RuntimeError('Build/test/game activity overlaps the WPR marker interval; see '+str(directory/'window-audit.json'))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('toolkit', type=Path)
    parser.add_argument('--mode', choices=['legacy', 'hyperpath'], required=True)
    args = parser.parse_args()
    print(json.dumps(audit_window(args.directory.resolve(), args.toolkit.resolve(), args.mode), indent=2))
