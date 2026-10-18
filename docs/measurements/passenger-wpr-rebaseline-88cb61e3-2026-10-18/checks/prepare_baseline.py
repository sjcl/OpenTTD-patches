"""Recreate the previous MSVC WPR configuration for immutable 88cb61e3."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(__file__).resolve().parent / 'source-8462e6cd'
BUILD = Path(__file__).resolve().parent / 'baseline'
BUILD.mkdir(exist_ok=True)
sys.path.insert(0, str(ROOT / 'utils'))
import passenger_precomputed_real_map as real

lease = Path(tempfile.gettempdir()) / 'openttd-passenger-routing-measurement.lock'
lock = real.PROFILE.acquire_measurement_lock(lease, 1800)
try:
    os.write(lock, json.dumps(dict(pid=os.getpid(), purpose='8462e6cd rebaseline WPR build', output=str(BUILD))).encode())
    real.PROFILE.wait_for_idle(1800, 5)
    config = json.loads((ROOT / 'build-wpr-88cb61e3/build-command.json').read_text())
    config['configure_command'][2] = str(SOURCE)
    config['configure_command'][4] = str(BUILD)
    config['checkout_commit'] = config['measurement_target'] = '8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4'
    config.pop('windows_build_fix_commit', None)
    config['native_test_command'][0] = str(BUILD / 'Release/openttd_test.exe')
    (BUILD / 'build-command.json').write_text(json.dumps(config,indent=2)+'\n')
    with (BUILD / 'configure.log').open('wb') as stream:
        subprocess.run(config['configure_command'], cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)
    print('Configured identical MSVC optimization and PDB flags', flush=True)
    for script, args in [
        ('passenger_estimated_prepare.py', [str(SOURCE), str(BUILD), '8462e6cd', '--cpu']),
        ('passenger_wpr_prepare.py', [str(BUILD)]),
    ]:
        subprocess.run([sys.executable, '-B', str(ROOT / 'utils' / script), *args], cwd=ROOT, check=True)
    with (BUILD / 'build.log').open('wb') as stream:
        subprocess.run(['cmake', '--build', str(BUILD), '--config', 'Release', '--target', 'openttd_test', 'openttd', '-j', '6'], cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)
    print('Build completed', flush=True)
    with (BUILD / 'native-all.log').open('wb') as stream:
        subprocess.run([str(BUILD / 'Release/openttd_test.exe'), '--reporter', 'compact'], cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)
    print((BUILD / 'native-all.log').read_text(errors='replace')[-400:], flush=True)
finally:
    os.close(lock)
    lease.unlink()
