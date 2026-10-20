"""Real Windows GUI cadence, zero simulation ticks, and real encrypted loopback joins."""
from prepare import ROOT, WORK, EVIDENCE, dump
from pathlib import Path
import argparse
import ctypes
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(ROOT / 'utils'))
import passenger_precomputed_real_map as real
PROFILE, CPU = real.PROFILE, real.CPU

def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
def text(path): return path.read_text(encoding='utf-8', errors='replace') if path.exists() else ''

def directory(output, label, workers, *, server=False, port=17640):
    output.mkdir(parents=True, exist_ok=False)
    package = WORK / 'input'
    for part in PROFILE.ASSET_DIRS:
        if part != 'lang' and (package / part).is_dir():
            shutil.copytree(package / part, output / part, copy_function=PROFILE.hardlink_or_copy)
    artifacts = WORK / 'artifacts-waiting' / label
    shutil.copytree(artifacts / 'lang', output / 'lang', copy_function=PROFILE.hardlink_or_copy)
    (output / 'scripts').mkdir()
    cfg = (package / 'openttd.cfg').read_text(encoding='utf-8')
    cfg += f'passenger_precomputed_workers = {workers}\n'
    cfg += f'[network]\nclient_name = waiting-{label}\nserver_game_type = local\nserver_port = {port}\npause_on_join = true\nmin_active_clients = 0\nframe_freq = 1\n'
    cfg += '[win32]\nwindow_maximize = false\n'
    (output / 'openttd.cfg').write_text(cfg, encoding='utf-8', newline='\n')
    return artifacts

def start(output, artifacts, arguments, role=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith('OPENTTD_')}
    env['OPENTTD_PASSENGER_MEASURE'] = '1'
    if role: env['OPENTTD_WAITING_ROLE'] = role
    command = [str(artifacts / 'Release/openttd.exe'), '-x', '-X', '-c', 'openttd.cfg', '-snull', '-mnull', '-dnet=4,sl=2', *arguments]
    info = subprocess.STARTUPINFO()
    info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    info.wShowWindow = 0  # First ShowWindow is hidden; same driver/cadence, no user interaction.
    streams = [(output / 'stdout.txt').open('wb'), (output / 'stderr.txt').open('wb')]
    dump(output / 'launch.json', {'command': command, 'environment': {k: v for k, v in env.items() if k.startswith('OPENTTD_')},
        'gui': 'win32 GDI, hidden through STARTF_USESHOWWINDOW; default threaded product GameLoop', 'resolution': '640x480'})
    child = subprocess.Popen(command, cwd=output, env=env, stdin=subprocess.PIPE, stdout=streams[0], stderr=streams[1],
        startupinfo=info, creationflags=subprocess.CREATE_NO_WINDOW)
    return child, streams

def monitor(child, output, allowed=(), timeout=900):
    started = time.perf_counter(); last_census = 0; overlap = []; observed = {}
    ready_at = None
    census_count = 0
    while True:
        for key, value in CPU.observe(child).items(): observed[key] = max(observed.get(key, 0), value)
        if (output / 'waiting-ready.json').is_file() and (output / 'save-integrity.json').is_file():
            try: ready, canonical = read(output / 'waiting-ready.json'), read(output / 'save-integrity.json')
            except (ValueError, OSError): pass
            else:
                ready_at = time.perf_counter() - started
                break
        if child.poll() is not None: break
        if time.monotonic() - last_census > 1:
            busy = [p for p in PROFILE.competing_processes() if p != f'openttd:{child.pid}' and p not in allowed]
            if busy: overlap.append({'elapsed': time.perf_counter() - started, 'processes': busy})
            census_count += 1
            dump(output / 'live-observer.json', {'census_count': census_count, 'contamination': overlap, 'peak': observed})
            last_census = time.monotonic()
        if time.perf_counter() - started > timeout:
            child.terminate(); child.wait(timeout=15)
            raise TimeoutError(f'Own waiting test exceeded {timeout}s: {output}')
        time.sleep(.05)
    if ready_at is None:
        raise RuntimeError(f'Game exited before readiness ({child.poll()}): {output}\n{text(output / "stderr.txt")[-1500:]}')
    dump(output / 'observed.json', {'ready_detected_seconds': ready_at, 'peak': observed, 'contamination': overlap,
        'census_count': census_count, 'binary_sha256': PROFILE.sha256(Path(read(output / 'launch.json')['command'][0])), 'pid': child.pid})
    assert not overlap, overlap
    assert ready['tick_before'] == ready['tick_after'] == canonical['state_ticks'] == 3201034745
    assert ready['ready'] and canonical['payments_valid']
    return ready, canonical

def local(label, mode, suffix, *, replay=False, workers=4):
    PROFILE.wait_for_idle(600, 5)
    output = WORK / 'waiting' / ('replay' if replay else 'initial') / suffix / mode
    artifacts = directory(output, label, workers)
    original = WORK / 'fixtures' / mode / 'result.sav' if replay else WORK / 'input/input.sav'
    shutil.copy2(original, output / 'input.sav')
    script = f'script {(output / "on-1-console.txt").as_posix()}\ngetfulldate\n'
    script += f'setting linkgraph.passenger_journey_hyperpath {str(mode == "hyperpath").lower()}\n'
    script += 'passenger_journey_stats waiting_arm\nscript\n'
    (output / 'scripts/game_start.scr').write_text(script, encoding='utf-8', newline='\n')
    print(f'Start GUI {suffix}/{mode} replay={replay} workers={workers}', flush=True)
    child, streams = start(output, artifacts, ['-g', 'input.sav', '-vwin32', '-r640x480'], 'singleplayer')
    try:
        ready, canonical = monitor(child, output)
        child.wait(timeout=45)
        assert child.returncode == 0
        dump(output / 'adoption.json', {'accepted': True, 'exit_code': 0, 'input_sha256': PROFILE.sha256(original),
            'label': label, 'mode': mode, 'replay': replay, 'tick_unchanged': True})
        print(f'Accepted GUI {suffix}/{mode}: process={ready["process_to_ready_ns"]/1e9:.3f}s preparation={ready["preparation_wall_ns"]/1e9:.3f}s', flush=True)
    finally:
        if child.poll() is None: child.terminate(); child.wait(timeout=15)
        for stream in streams: stream.close()

def initial():
    for label, suffix in [('baseline', 'A1'), ('candidate', 'B1')]:
        for mode in ['legacy', 'hyperpath']:
            # Preserve the completed raw measurement after an observer exit race.
            if suffix == 'A1' and mode == 'legacy' and (WORK / 'waiting/initial/A1/legacy/waiting-ready.json').exists(): continue
            local(label, mode, suffix)

def initial_repeat():
    local('baseline', 'legacy', 'A2')

def replay():
    for label, suffix in [('baseline', 'A1'), ('candidate', 'B1')]:
        for mode in ['legacy', 'hyperpath']: local(label, mode, suffix, replay=True)

def repeat():
    # Repeat the priority retained-generation case in reverse order.
    for label, suffix in [('candidate', 'B2'), ('baseline', 'A2')]:
        local(label, 'hyperpath', suffix, replay=True)

def pilot(): local('candidate', 'hyperpath', 'pilot')

def workers8():
    local('candidate', 'hyperpath', 'workers8', workers=8)
    local('candidate', 'hyperpath', 'workers8', replay=True, workers=8)

def multiplayer():
    for label in ['baseline', 'candidate']:
        PROFILE.wait_for_idle(600, 5)
        root = WORK / 'waiting' / 'multiplayer-clean' / label
        host_dir, guest_dir = root / 'server', root / 'client'
        artifacts = directory(host_dir, label, 4, server=True)
        directory(guest_dir, label, 4)
        fixture = WORK / 'fixtures-mp/hyperpath/result.sav'
        shutil.copy2(fixture, host_dir / 'input.sav')
        (host_dir / 'scripts/game_start.scr').write_text('pause\npassenger_journey_stats wait_saved_replay\npassenger_journey_stats save_integrity\n', encoding='utf-8', newline='\n')
        host, hs = start(host_dir, artifacts, ['-D', '127.0.0.1:17640', '-g', 'input.sav'])
        guest = None; gs = []
        print(f'Start loopback host {label}', flush=True)
        try:
            until = time.monotonic() + 900
            while not (host_dir / 'save-integrity.json').is_file():
                if host.poll() is not None: raise RuntimeError(f'Host exited: {host_dir}')
                if time.monotonic() > until: raise TimeoutError('Host preparation')
                time.sleep(.1)
            host_state = read(host_dir / 'save-integrity.json')
            assert host_state['completed_rows'] <= 1 and host_state['retained_rows'] > 0
            print(f'Connect GUI client {label}', flush=True)
            guest, gs = start(guest_dir, artifacts, ['-vwin32', '-r640x480', '-n', '127.0.0.1:17640#255'], 'client')
            ready, guest_state = monitor(guest, guest_dir, [f'openttd:{host.pid}'])
            mismatch = {k: [host_state[k], guest_state.get(k)] for k in host_state if host_state[k] != guest_state.get(k)}
            dump(root / 'canonical-comparison.json', {'mismatches': mismatch, 'equal': not mismatch})
            assert not mismatch, mismatch
            dump(root / 'adoption.json', {'accepted': True, 'label': label, 'mode': 'hyperpath',
                'kind': 'Real encrypted TCP loopback, spectator, first successful sync closes the join window',
                'tick_unchanged': True, 'input_sha256': PROFILE.sha256(fixture)})
            print(f'Accepted loopback {label}: join={ready["process_to_ready_ns"]/1e9:.3f}s preparation={ready["preparation_wall_ns"]/1e9:.3f}s', flush=True)
        finally:
            if guest is not None and guest.poll() is None: guest.terminate(); guest.wait(timeout=15)
            if host.poll() is None:
                try: host.stdin.write(b'quit\n'); host.stdin.flush(); host.wait(timeout=30)
                except (OSError, subprocess.TimeoutExpired): host.terminate(); host.wait(timeout=15)
            for stream in hs + gs: stream.close()

if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('phase', choices=['pilot', 'initial', 'initial_repeat', 'replay', 'repeat', 'multiplayer', 'workers8'])
    args = parser.parse_args()
    globals()[args.phase]()
