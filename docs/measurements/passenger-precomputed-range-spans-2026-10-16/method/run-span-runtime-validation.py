"""After the adopted checkpoint, verify real retained updates and early save readback."""
import argparse,hashlib,json,os,pathlib,shutil,subprocess,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'utils'))
import passenger_precomputed_real_map as real
data=root/'build-msvc';build=root/'build-runtime-matched';out=data/'span-runtime-validation';out.mkdir(exist_ok=True)
checkpoint='d7af532165489277745dafa23f7555ac9640be0d'
assert subprocess.check_output(['git','show',checkpoint+':src/passenger_precomputed.cpp'],cwd=root).replace(b'\r\n',b'\n')==(root/'src/passenger_precomputed.cpp').read_bytes().replace(b'\r\n',b'\n')
lock_path=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
lock=real.PROFILE.acquire_measurement_lock(lock_path,14400)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(out),purpose='adopted range kernel real retained state and save readback')).encode())
def run(cmd,name):
 print(name,flush=True)
 with (out/name).open('wb') as stream:
  subprocess.run(list(map(str,cmd)),cwd=root,stdout=stream,stderr=subprocess.STDOUT,check=True,
   creationflags=subprocess.CREATE_NO_WINDOW)
try:
 real.PROFILE.wait_for_idle(300,5)
 binary=out/'cpu-binary';binary.mkdir(exist_ok=True)
 if '--reuse-binary' not in sys.argv:
  run(['cmake','-S',root,'-B',build,'-DCMAKE_PROJECT_INCLUDE='],'configure.log')
  run([sys.executable,'-B',root/'utils/passenger_estimated_prepare.py',root,build,checkpoint,'--cpu'],'prepare.log')
  run(['cmake','--build',build,'--config','Release','--target','openttd','--parallel','12'],'build.log')
  shutil.copy2(build/'Release/openttd.exe',binary/'openttd.exe')
 sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
 provenance=dict(checkpoint=checkpoint,binary_sha256=sha(binary/'openttd.exe'),
  product_cpp_sha256=sha(root/'src/passenger_precomputed.cpp'),compiler=real.PROFILE.compiler_info(build),
  validation_only=True,performance_comparison=False)
 if '--reuse-binary' in sys.argv:
  previous=json.loads((out/'provenance.json').read_text())
  assert all(previous[k]==provenance[k] for k in ['checkpoint','binary_sha256','product_cpp_sha256'])
 else:(out/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
 target=out/'retained-actual'
 args=argparse.Namespace(binary=binary/'openttd.exe',package=data/'package',output=target,input=data/'runtime-common-input/result.sav',
  lang=data/'lang',ticks=2048,timeout=900,resume=True,fixture_timetable=False,fixture_estimated=False,cold_precomputed=False,
  precomputed=True,warm_precomputed=False,rebuild_precomputed=True,quality_probe=False,unpause_before_save=False,
  validation_only=True,affinity_mask=None,main_thread_affinity_mask=1,observe_host=False,wait_background_ready=True)
 # Correctness-only runs own a separate per-PID validation lock. Keep the outer
 # shared lease throughout, without asking the utility to borrow that lock.
 real.run(args)
 expected=json.loads((data/'runtime-background-ready-retained-new-a/save-integrity.json').read_text())
 actual=json.loads((target/'save-integrity.json').read_text())
 mismatch={k:[expected.get(k),actual.get(k)] for k in expected.keys()|actual.keys() if expected.get(k)!=actual.get(k)}
 (out/'canonical.json').write_text(json.dumps(dict(reference='runtime-background-ready-retained-new-a',mismatches=mismatch),indent=2)+'\n')
 assert not mismatch,mismatch
 run([sys.executable,'-B',root/'utils/passenger_precomputed_diagnostic_readback.py',binary/'openttd.exe',
  data/'package',data/'lang',out/'readback',target],'readback.log')
 (out/'completed.json').write_text(json.dumps(dict(success=True,canonical_equal=True,performance_comparison=False))+'\n')
finally:
 try:
  run(['cmake','-S',root,'-B',build,'-DCMAKE_PROJECT_INCLUDE='],'restore-configure.log')
  run(['cmake','--build',build,'--config','Release','--target','openttd','--parallel','12'],'restore-build.log')
  assert not any(x in (build/'openttd_lib.vcxproj').read_text(encoding='utf-8-sig') for x in ['save-integrity','real-map-driver','table-build'])
  (out/'restored.json').write_text(json.dumps(dict(restored=True,normal_binary_sha256=hashlib.sha256((build/'Release/openttd.exe').read_bytes()).hexdigest()))+'\n')
 finally:
  os.close(lock);lock_path.unlink()
