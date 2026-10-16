import argparse,hashlib,json,os,pathlib,shutil,subprocess,sys,tempfile,time
root=pathlib.Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'utils'))
import passenger_precomputed_real_map as real
data=root/'build-msvc';build=root/'build-runtime-matched';deadline=time.monotonic()+3600
print('Waiting for the reverse-order comparison before isolated empty-retained experiment.',flush=True)
while not (data/'runtime-main-reverse-summary.json').exists():
 if time.monotonic()>deadline:raise TimeoutError('Reverse-order comparison not completed')
 time.sleep(5)
path=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock';lock=real.PROFILE.acquire_measurement_lock(path,3600)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(data/'runtime-empty-retained'),purpose='empty retained table copied-TU experiment')).encode())
def run(cmd,name):
 print('Running '+name,flush=True)
 with (build/name).open('wb') as f:subprocess.run(list(map(str,cmd)),cwd=root,stdout=f,stderr=subprocess.STDOUT,check=True,creationflags=subprocess.CREATE_NO_WINDOW)
try:
 real.PROFILE.wait_for_idle(300,5)
 rev=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
 run([sys.executable,'-B',root/'utils/passenger_estimated_prepare.py',root,build,rev,'--cpu'],'empty-retained-prepare.log')
 p=build/'save-integrity/passenger_service_database.cpp';s=p.read_text(encoding='utf-8');a='void PassengerServiceDatabase::ClearRetainedPrecomputed()\n{\n\tthis->retained_precomputed = {};';assert s.count(a)==1
 s=s.replace(a,'void PassengerServiceDatabase::ClearRetainedPrecomputed()\n{\n\tif (this->retained_precomputed_revision != UINT64_MAX) this->retained_precomputed = {};')
 p.write_text(s,encoding='utf-8')
 run(['cmake','--build',build,'--config','Release','--target','openttd','--parallel','12'],'empty-retained-build.log')
 out=build/'empty-retained-binary';out.mkdir(exist_ok=True);shutil.copy2(build/'Release/openttd.exe',out/'openttd.exe');shutil.copy2(p,out/'passenger_service_database.cpp')
 (out/'manifest.json').write_text(json.dumps(dict(source_head=rev,experiment='Skip empty retained table array assignment; always reset all scalar flags as before.',binary_sha256=hashlib.sha256((out/'openttd.exe').read_bytes()).hexdigest(),copied_database_sha256=hashlib.sha256(p.read_bytes()).hexdigest()),indent=2)+'\n')
 for label,replacing in [('steady',False),('retained',True)]:
  args=argparse.Namespace(binary=out/'openttd.exe',package=data/'package',output=data/('runtime-empty-retained-'+label),input=data/'runtime-common-input/result.sav',lang=data/'lang',ticks=2048,timeout=900,resume=True,fixture_timetable=False,fixture_estimated=False,cold_precomputed=False,precomputed=True,warm_precomputed=not replacing,rebuild_precomputed=replacing,quality_probe=False,unpause_before_save=False,validation_only=False,affinity_mask=None,main_thread_affinity_mask=1)
  real.run(args,measurement_lock_fd=lock)
 real.summarize(argparse.Namespace(output=data/'runtime-empty-retained-summary.json',runs=[data/'runtime-empty-retained-steady',data/'runtime-empty-retained-retained']))
 print('Empty-retained experiment completed.',flush=True)
finally:
 try:
  run(['cmake','-S',root,'-B',build,'-DCMAKE_PROJECT_INCLUDE='],'empty-retained-restore-configure.log')
  run(['cmake','--build',build,'--config','Release','--target','openttd','--parallel','12'],'empty-retained-restore-build.log')
 finally:
  os.close(lock);path.unlink()
