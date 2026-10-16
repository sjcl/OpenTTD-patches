"""Compare engines in one measurement image; copied DB TU only, no product edits."""
import argparse, hashlib, json, os, pathlib, shutil, subprocess, sys, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root/'utils'))
import passenger_precomputed_real_map as real
data=root/'build-msvc';build=root/'build-runtime-matched';out=data/'same-image-control'
out.mkdir(exist_ok=True)
lock_path=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
print('Waiting for actual shared-lock handoff.',flush=True)
lock=real.PROFILE.acquire_measurement_lock(lock_path,14400)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(out),purpose='same-image legacy/integrated steady-tick control')).encode())
key='OPENTTD_PASSENGER_ENGINE_CONTROL'
assert key not in os.environ
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()

def run(cmd,log):
 print('Running '+log,flush=True)
 with (out/log).open('wb') as f:
  subprocess.run(list(map(str,cmd)),cwd=root,stdout=f,stderr=subprocess.STDOUT,
                 check=True,creationflags=subprocess.CREATE_NO_WINDOW)

def configure(log):
 run(['cmake','-S',root,'-B',build,'-DCMAKE_PROJECT_INCLUDE='],log)

try:
 real.PROFILE.wait_for_idle(300,5)
 # This control is intentionally based on the fixed, unmodified product.
 originals={'passenger_precomputed.cpp':'d89fb2a43a0614a9255addcd31a9cdd69cf47e82d5aceec5fd06ba246883aaec',
            'passenger_precomputed.h':'8e51935b3f5855edca482da3c887bcba34bde7e83fdee8c95a6edd54123d4bd4'}
 assert all(sha(root/'src'/n)==v for n,v in originals.items())
 revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
 configure('configure.log')
 run([sys.executable,'-B',root/'utils/passenger_estimated_prepare.py',root,build,revision,'--cpu'],'prepare.log')
 p=build/'save-integrity/passenger_service_database.cpp';s=p.read_text(encoding='utf-8')
 a='this->precomputed_optimized = true; this->precomputed_integrated = true;\n\tthis->precomputed_revision ='
 assert s.count(a)==1
 s=s.replace(a,'this->precomputed_optimized = getenv("'+key+'") == nullptr; this->precomputed_integrated = this->precomputed_optimized;\n\tthis->precomputed_revision =')
 a='!this->precomputed_estimates || !this->precomputed_optimized || !this->precomputed_integrated || this->precomputed_invalidated'
 assert s.count(a)==1
 # Omit only the legacy-engine upgrade trigger in BOTH modes of this copied
 # TU. Otherwise legacy rows would be immediately replaced after warming.
 s=s.replace(a,'!this->precomputed_estimates || this->precomputed_invalidated')
 p.write_text(s,encoding='utf-8')
 run(['cmake','--build',build,'--config','Release','--target','openttd','--parallel','12'],'build.log')
 binary=out/'binary';binary.mkdir(exist_ok=True)
 shutil.copy2(build/'Release/openttd.exe',binary/'openttd.exe')
 shutil.copy2(p,out/'passenger_service_database.cpp')
 (out/'provenance.json').write_text(json.dumps(dict(source_head=revision,product_sha256=originals,
  binary_sha256=sha(binary/'openttd.exe'),copied_database_sha256=sha(p),
  compiler=real.PROFILE.compiler_info(build),environment_control=key,
  runtime_tool_sha256=sha(root/'utils/passenger_precomputed_real_map.py'),
  note='Same binary for all cases. Setup selects the existing replay engine. Copied TU omits immediate engine upgrade in both modes. No product source edits.'),indent=2)+'\n')
 checks={};runs=[]
 for label,legacy in [('integrated-a',False),('legacy-a',True),('legacy-b',True),('integrated-b',False)]:
  if legacy:os.environ[key]='legacy'
  else:os.environ.pop(key,None)
  target=data/('runtime-same-image-'+label)
  print('Measuring '+label,flush=True)
  args=argparse.Namespace(binary=binary/'openttd.exe',package=data/'package',output=target,
   input=data/'runtime-common-input/result.sav',lang=data/'lang',ticks=2048,timeout=900,resume=True,
   fixture_timetable=False,fixture_estimated=False,cold_precomputed=False,precomputed=True,
   warm_precomputed=True,rebuild_precomputed=False,quality_probe=False,unpause_before_save=False,
   validation_only=False,affinity_mask=None,main_thread_affinity_mask=1)
  real.run(args,measurement_lock_fd=lock);runs.append(target)
  reference='runtime-reference-fresh-steady' if legacy else 'runtime-main-reverse-new'
  old=json.loads((data/reference/'save-integrity.json').read_text());new=json.loads((target/'save-integrity.json').read_text())
  # The same-image legacy control still writes the new DB format/identity.
  ignored={'database_digest'} if legacy else set()
  mismatch={k:[old.get(k),new.get(k)] for k in old.keys()|new.keys() if k not in ignored and old.get(k)!=new.get(k)}
  checks[label]=dict(reference=reference,ignored=sorted(ignored),mismatches=mismatch)
  (out/'canonical.json').write_text(json.dumps(checks,indent=2)+'\n');assert not mismatch,mismatch
 real.summarize(argparse.Namespace(output=data/'runtime-same-image-summary.json',runs=runs))
 (out/'completed.json').write_text(json.dumps(dict(success=True,product_changed=False))+'\n')
finally:
 os.environ.pop(key,None)
 try:
  configure('restore-configure.log')
  run(['cmake','--build',build,'--config','Release','--target','openttd','--parallel','12'],'restore-build.log')
 finally:
  os.close(lock);lock_path.unlink()
