"""Reuse validated base/current images for a balanced retained-update comparison.

Launch only after the steady comparison succeeds and the next lease is agreed.
"""
import argparse,hashlib,json,os,pathlib,shutil,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'utils'))
import passenger_precomputed_real_map as real
data=root/'build-msvc';prepared=data/'background-ready-control';out=data/'background-ready-retained';out.mkdir(exist_ok=True)
assert json.loads((prepared/'completed.json').read_text())['success']
provenance=json.loads((prepared/'provenance.json').read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
lock_path=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
print('Waiting for actual retained-update measurement handoff.',flush=True)
lock=real.PROFILE.acquire_measurement_lock(lock_path,14400)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(out),purpose='background-ready retained update old/new balanced comparison')).encode())
try:
 real.PROFILE.wait_for_idle(300,5)
 for version in ['old','new']:
  assert sha(prepared/(version+'-binary')/'openttd.exe')==provenance[version]['binary_sha256']
 (out/'provenance.json').write_text(json.dumps(dict(prepared_images='background-ready-control/provenance.json',
  binaries={version:provenance[version]['binary_sha256'] for version in ['old','new']},
  product_changed=False,order=['old-a','new-a','new-b','old-b'],
  tools={p.name:sha(p) for p in [root/'utils/passenger_precomputed_real_map.py',root/'utils/passenger_host_observer.py']}),indent=2)+'\n')
 captured=out/'measurement-sources';captured.mkdir(exist_ok=True)
 for name in ['passenger_precomputed_real_map.py','passenger_host_observer.py']:
  shutil.copy2(root/'utils'/name,captured/name)
 runs=[];checks={}
 for label in ['old-a','new-a','new-b','old-b']:
  version=label.split('-')[0];target=data/('runtime-background-ready-retained-'+label)
  print('Measuring retained '+label,flush=True)
  args=argparse.Namespace(binary=prepared/(version+'-binary')/'openttd.exe',package=data/'package',output=target,
   input=data/'runtime-common-input/result.sav',lang=data/'lang',ticks=2048,timeout=900,resume=True,
   fixture_timetable=False,fixture_estimated=False,cold_precomputed=False,precomputed=True,
   warm_precomputed=False,rebuild_precomputed=True,quality_probe=False,unpause_before_save=False,
   validation_only=False,affinity_mask=None,main_thread_affinity_mask=1,observe_host=True,wait_background_ready=True)
  real.run(args,measurement_lock_fd=lock);runs.append(target)
  baseline='runtime-reference-fresh-retained' if version=='old' else 'runtime-matched-new-retained'
  a=json.loads((data/baseline/'save-integrity.json').read_text());b=json.loads((target/'save-integrity.json').read_text())
  mismatch={k:[a.get(k),b.get(k)] for k in a.keys()|b.keys() if a.get(k)!=b.get(k)}
  checks[label]=dict(reference=baseline,mismatches=mismatch)
  (out/'canonical.json').write_text(json.dumps(checks,indent=2)+'\n');assert not mismatch,mismatch
 real.summarize(argparse.Namespace(output=data/'runtime-background-ready-retained-summary.json',runs=runs))
 (out/'completed.json').write_text(json.dumps(dict(success=True,product_changed=False))+'\n')
finally:
 os.close(lock);lock_path.unlink()
