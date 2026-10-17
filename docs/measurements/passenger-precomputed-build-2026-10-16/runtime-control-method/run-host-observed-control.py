"""Read-only host observations, using the exact previous control executable."""
import argparse,ctypes,hashlib,json,os,pathlib,subprocess,sys,tempfile,time
root=pathlib.Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'utils'))
import passenger_precomputed_real_map as real
from passenger_host_observer import HostObserver
data=root/'build-msvc';out=data/'host-observed-control';out.mkdir(exist_ok=True)
binary=data/'same-image-control/binary/openttd.exe'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
lock_path=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
print('Peer quality lease observed. Waiting for actual handoff.',flush=True)
lock=real.PROFILE.acquire_measurement_lock(lock_path,14400)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(out),purpose='read-only host observations and same-image repeated controls')).encode())
key='OPENTTD_PASSENGER_ENGINE_CONTROL';assert key not in os.environ
try:
 real.PROFILE.wait_for_idle(300,5)
 assert sha(binary)=='1097338f0ea711eb9b5af992cf77abe72c2fc503726b3edfefec18439e677280'
 # Exercise native layouts and PDH availability before launching any benchmark.
 kernel=ctypes.WinDLL('kernel32');kernel.GetCurrentProcess.restype=ctypes.c_void_p
 observer=HostObserver(kernel.GetCurrentProcess(),os.getpid(),kernel.GetCurrentThreadId())
 try:
  for n in range(2):time.sleep(1.1);observer.sample(5+n,'preparation')
  probe=observer.result()
 finally:observer.close()
 (out/'observer-probe.json').write_text(json.dumps(probe,indent=2)+'\n')
 assert probe['samples'][-1]['counters']['cpu0_performance_percent']['value'] is not None,probe
 assert probe['samples'][-1]['counters']['cpu0_frequency_mhz']['value'] is not None,probe
 print('Read-only host observer native probe passed.',flush=True)
 with (out/'save-byte-comparison.log').open('wb') as log:
  subprocess.run([sys.executable,'-B',str(data/'compare-control-saves.py')],cwd=root,
                 stdout=log,stderr=subprocess.STDOUT,check=True,creationflags=subprocess.CREATE_NO_WINDOW)
 (out/'provenance.json').write_text(json.dumps(dict(binary_sha256=sha(binary),
  original_control='same-image-control/provenance.json',product_changed=False,
  observer_sha256=sha(root/'utils/passenger_host_observer.py'),runtime_tool_sha256=sha(root/'utils/passenger_precomputed_real_map.py'),
  order=['legacy-c','integrated-c','integrated-d','legacy-d'],policy_changes=False,
  sources=['https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw',
           'https://learn.microsoft.com/en-us/windows/win32/perfctrs/collecting-performance-data',
           'https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-process_power_throttling_state']),indent=2)+'\n')
 checks={};runs=[]
 for label,legacy in [('legacy-c',True),('integrated-c',False),('integrated-d',False),('legacy-d',True)]:
  if legacy:os.environ[key]='legacy'
  else:os.environ.pop(key,None)
  target=data/('runtime-host-observed-'+label);print('Measuring '+label,flush=True)
  args=argparse.Namespace(binary=binary,package=data/'package',output=target,input=data/'runtime-common-input/result.sav',
   lang=data/'lang',ticks=2048,timeout=900,resume=True,fixture_timetable=False,fixture_estimated=False,cold_precomputed=False,
   precomputed=True,warm_precomputed=True,rebuild_precomputed=False,quality_probe=False,unpause_before_save=False,
   validation_only=False,affinity_mask=None,main_thread_affinity_mask=1,observe_host=True)
  real.run(args,measurement_lock_fd=lock);runs.append(target)
  baseline='runtime-same-image-legacy-a' if legacy else 'runtime-same-image-integrated-a'
  old=json.loads((data/baseline/'save-integrity.json').read_text());new=json.loads((target/'save-integrity.json').read_text())
  mismatch={k:[old.get(k),new.get(k)] for k in old.keys()|new.keys() if old.get(k)!=new.get(k)}
  expected=json.loads((data/baseline/'process.json').read_text())['result_sha256']
  checks[label]=dict(reference=baseline,mismatches=mismatch,entire_save_hash_matches=sha(target/'result.sav')==expected)
  (out/'canonical.json').write_text(json.dumps(checks,indent=2)+'\n')
  assert not mismatch and checks[label]['entire_save_hash_matches'],checks[label]
 real.summarize(argparse.Namespace(output=data/'runtime-host-observed-summary.json',runs=runs))
 (out/'completed.json').write_text(json.dumps(dict(success=True,product_changed=False,read_only_observations=True))+'\n')
 print('All host-observed controls completed.',flush=True)
finally:
 os.environ.pop(key,None);os.close(lock);lock_path.unlink()
