import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time

root=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'utils'))
import passenger_precomputed_real_map as real
data=root/'build-msvc';build=root/'build-runtime-matched'
print('Waiting for the same-core comparison, then the shared host reservation.',flush=True)
deadline=time.monotonic()+3600
while not (data/'runtime-core-control-summary.json').is_file():
    if time.monotonic()>deadline: raise TimeoutError('Same-core comparison has not completed')
    time.sleep(5)
path=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
lock=real.PROFILE.acquire_measurement_lock(path,3600)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(build),purpose='regression fixture and final MP/readback')).encode())
def run(args,log):
    print('Running: '+' '.join(map(str,args)),flush=True)
    with (build/log).open('wb') as f:
        subprocess.run(list(map(str,args)),cwd=root,stdout=f,stderr=subprocess.STDOUT,
                       check=True,creationflags=subprocess.CREATE_NO_WINDOW)
try:
    real.PROFILE.wait_for_idle(300,5)
    run(['cmake','--build',build,'--config','Release','--target','regression_files','--parallel','8'],'regression-fixture-build.log')
    run(['ctest','--test-dir',build,'-C','Release','--rerun-failed','--output-on-failure','--parallel','4'],'ctest-retry.log')
    run([sys.executable,'-B',root/'utils/passenger_precomputed_join.py',build/'mp-binary/openttd.exe',
         data/'package',data/'toggle-mp-input-ready/result.sav',data/'matched-mp-toggle',data/'lang',
         '--port','17533','--frame-freq','20','--ticks','1024','--observe-timeout','300',
         '--initial-build','--toggle-precomputed'],'mp-toggle.log')
    run([sys.executable,'-B',root/'utils/passenger_precomputed_diagnostic_readback.py',build/'mp-binary/openttd.exe',
         data/'package',data/'lang',data/'matched-readback',data/'runtime-matched-new-steady',
         data/'runtime-matched-new-retained'],'readback.log')
    for file in ['openttd_lib.vcxproj','openttd_test.vcxproj']:
        s=(build/file).read_text(encoding='utf-8-sig')
        assert not any(p in s for p in ['save-integrity','real-map-driver','table-build'])
    (build/'completed.json').write_text(json.dumps(dict(success=True,
        normal_binary_sha256=hashlib.sha256((build/'Release/openttd.exe').read_bytes()).hexdigest()))+'\n')
    print('All matching-configuration validation completed.',flush=True)
finally:
    os.close(lock);path.unlink()
