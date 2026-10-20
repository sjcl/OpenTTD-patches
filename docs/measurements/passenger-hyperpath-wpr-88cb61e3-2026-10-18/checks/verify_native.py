from pathlib import Path
import os
import json
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'utils'))
import passenger_precomputed_real_map as real
lease = Path(tempfile.gettempdir()) / 'openttd-passenger-routing-measurement.lock'
lock = real.PROFILE.acquire_measurement_lock(lease, 1800)
try:
    os.write(lock, json.dumps(dict(pid=os.getpid(), purpose='88cb61e3 native tests', output=str(BUILD))).encode())
    # These assertions are correctness checks; their duration is not performance evidence.
    with (BUILD / 'native-all.log').open('wb') as stream:
        subprocess.run([str(BUILD / 'Release/openttd_test.exe'), '--reporter', 'compact'], cwd=ROOT,
            stdout=stream, stderr=subprocess.STDOUT, check=True)
    print((BUILD / 'native-all.log').read_text(errors='replace')[-500:], flush=True)
finally:
    os.close(lock)
    lease.unlink()
