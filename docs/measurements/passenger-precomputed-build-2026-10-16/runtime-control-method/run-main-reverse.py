import argparse,json,os,pathlib,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'utils'))
import passenger_precomputed_real_map as real
data=root/'build-msvc';build=root/'build-runtime-matched'
path=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
print('Waiting for shared reservation; reverse the previous run order.',flush=True)
lock=real.PROFILE.acquire_measurement_lock(path,3600)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(data/'runtime-main-reverse'),purpose='reverse-order main-thread comparison')).encode())
try:
 old=pathlib.Path('C:/Users/bebe/.codex/worktrees/dae2/OpenTTD-patches/build-estimated/cpu-through-final/openttd.exe')
 for label,binary,lang in [('new',build/'cpu-binary/openttd.exe',data/'lang'),('old',old,old.parent/'lang')]:
  print('Reverse-order measurement: '+label,flush=True)
  args=argparse.Namespace(binary=binary,package=data/'package',output=data/('runtime-main-reverse-'+label),input=data/'runtime-common-input/result.sav',lang=lang,ticks=2048,timeout=900,resume=True,fixture_timetable=False,fixture_estimated=False,cold_precomputed=False,precomputed=True,warm_precomputed=True,rebuild_precomputed=False,quality_probe=False,unpause_before_save=False,validation_only=False,affinity_mask=None,main_thread_affinity_mask=1)
  real.run(args,measurement_lock_fd=lock)
 real.summarize(argparse.Namespace(output=data/'runtime-main-reverse-summary.json',runs=[data/'runtime-main-reverse-new',data/'runtime-main-reverse-old']))
 print('Reverse-order comparison completed.',flush=True)
finally:
 os.close(lock);path.unlink()
