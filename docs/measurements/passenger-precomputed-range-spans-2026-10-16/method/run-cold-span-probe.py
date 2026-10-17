"""Filter the next kernel on a declared row prefix before expensive validation."""
import hashlib,json,os,pathlib,shutil,subprocess,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'utils'))
import passenger_precomputed_real_map as real
build=root/'build-msvc';out=build/'cold-span-probe';out.mkdir(exist_ok=True)
lock_path=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
lock=real.PROFILE.acquire_measurement_lock(lock_path,14400)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(out),purpose='cold accept bounded-span prefix probe')).encode())
def run(cmd,name,env=None):
 print(name,flush=True)
 with (out/name).open('wb') as stream:
  subprocess.run(list(map(str,cmd)),cwd=root,stdout=stream,stderr=subprocess.STDOUT,check=True,
   creationflags=subprocess.CREATE_NO_WINDOW,env=env)
try:
 real.PROFILE.wait_for_idle(300,5)
 run([sys.executable,'-B',root/'utils/passenger_table_build_prepare.py',out,'--reference',
  '5dc3b990ffadf8e134ba3318b1b03647356a4125','--heap','product','--range-split','--timing-only'],'prepare.log')
 run(['cmake','-S',root,'-B',build,'-DCMAKE_PROJECT_INCLUDE='+(out/'table-build/benchmark.cmake').as_posix()],'configure.log')
 run(['cmake','--build',build,'--config','Release','--target','openttd_test','--parallel','12'],'build.log')
 test=build/'Release/openttd_test.exe'
 run([test,'[.table-ranges]'],'boundaries.log')
 for name,reverse in [('forward',False),('reverse',True)]:
  env=dict(os.environ,OPENTTD_TABLE_RANGE_SPLIT='1')
  if reverse:env['OPENTTD_TABLE_REVERSE_ORDER']='1'
  run([sys.executable,'-B',root/'utils/passenger_table_build_run.py',test,out/name,'--fixture',
   build/'real-fixture','--skip-plan','--destinations','512'],name+'-runner.log',env)
 (out/'completed.json').write_text(json.dumps(dict(success=True,scope='512-destination prefix only, not final adoption evidence'))+'\n')
finally:
 try:
  run(['cmake','-S',root,'-B',build,'-DCMAKE_PROJECT_INCLUDE='],'restore-configure.log')
  run(['cmake','--build',build,'--config','Release','--target','openttd','openttd_test','--parallel','12'],'restore-build.log')
  assert 'table-build' not in (build/'openttd_test.vcxproj').read_text(encoding='utf-8-sig')
  (out/'restored.json').write_text(json.dumps(dict(restored=True,product_sha256=hashlib.sha256((root/'src/passenger_precomputed.cpp').read_bytes()).hexdigest()))+'\n')
 finally:
  os.close(lock);lock_path.unlink()
