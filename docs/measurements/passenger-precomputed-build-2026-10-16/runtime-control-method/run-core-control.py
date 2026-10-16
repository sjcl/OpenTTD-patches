import argparse
import json
import os
import pathlib
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root/'utils'))
import passenger_precomputed_real_map as real

data = root/'build-msvc'
build = root/'build-runtime-matched'
lock_path = pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
print('Waiting for the shared host reservation; no other execution is stopped.', flush=True)
lock = real.PROFILE.acquire_measurement_lock(lock_path, 3600)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(data/'runtime-core-control'),purpose='same logical CPU comparison')).encode())
try:
    old = pathlib.Path('C:/Users/bebe/.codex/worktrees/dae2/OpenTTD-patches/build-estimated/cpu-through-final/openttd.exe')
    for label,binary,lang in [('old',old,old.parent/'lang'),('new',build/'cpu-binary/openttd.exe',data/'lang')]:
        print('Measuring the same CPU: '+label,flush=True)
        args=argparse.Namespace(binary=binary,package=data/'package',output=data/('runtime-core-control-'+label),
            input=data/'runtime-common-input/result.sav',lang=lang,ticks=2048,timeout=1500,resume=True,
            fixture_timetable=False,fixture_estimated=False,cold_precomputed=False,precomputed=True,
            warm_precomputed=True,rebuild_precomputed=False,quality_probe=False,
            unpause_before_save=False,validation_only=False,affinity_mask=1)
        real.run(args,measurement_lock_fd=lock)
    real.summarize(argparse.Namespace(output=data/'runtime-core-control-summary.json',
        runs=[data/'runtime-core-control-old',data/'runtime-core-control-new']))
    print('Same CPU comparison completed.',flush=True)
finally:
    os.close(lock)
    lock_path.unlink()
