"""Own the shared lease through native correctness, timings and restoration."""
import hashlib,json,os,pathlib,shutil,subprocess,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'utils'))
import passenger_precomputed_real_map as real
profile=real.PROFILE
build=root/'build-msvc';out=build/'split-range-validation';out.mkdir(exist_ok=True)
lock_path=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
lock=profile.acquire_measurement_lock(lock_path,14400)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(out),purpose='split range scalar/SIMD exact-work validation')).encode())
def run(cmd,name,env=None):
 print(name,flush=True)
 with (out/name).open('wb') as stream:
  subprocess.run(list(map(str,cmd)),cwd=root,stdout=stream,stderr=subprocess.STDOUT,check=True,
   creationflags=subprocess.CREATE_NO_WINDOW,env=env)
def configure(include,name):run(['cmake','-S',root,'-B',build,'-DCMAKE_PROJECT_INCLUDE='+include],name)
def compile(name):run(['cmake','--build',build,'--config','Release','--target','openttd','openttd_test','--parallel','12'],name)
try:
 profile.wait_for_idle(300,5)
 configure('','product-configure.log');compile('product-build.log')
 run(['ctest','--test-dir',build,'-C','Release','--output-on-failure','--parallel','4'],'ctest.log')
 prepare=[sys.executable,'-B',root/'utils/passenger_table_build_prepare.py',build/'split-range-counted',
  '--reference','5dc3b990ffadf8e134ba3318b1b03647356a4125','--heap','product','--range-split']
 run(prepare,'prepare-counted.log')
 shutil.copy2(build/'split-range-counted/table-build/source.json',out/'counted-source.json')
 configure((build/'split-range-counted/table-build/benchmark.cmake').as_posix(),'counted-configure.log')
 compile('counted-build.log')
 test=build/'Release/openttd_test.exe'
 run([test,'[.table-ranges]'],'range-boundaries.log')
 run([test,'[.table-build]'],'synthetic.log')
 for quantum in ['1','4','5']:
  env=dict(os.environ,OPENTTD_TABLE_QUANTUM=quantum)
  run([test,'[.table-build]'],'synthetic-budget-'+quantum+'.log',env)
 run([sys.executable,'-B',root/'utils/passenger_table_build_run.py',test,out/'all-plan',
  '--fixture',build/'real-fixture'],'all-plan-runner.log')
 run(prepare+['--timing-only'],'prepare-timing.log')
 shutil.copy2(build/'split-range-counted/table-build/source.json',out/'timing-source.json')
 configure((build/'split-range-counted/table-build/benchmark.cmake').as_posix(),'timing-configure.log')
 compile('timing-build.log')
 shutil.copy2(test,out/'openttd_test-timing.exe')
 for label,reverse in [('forward',False),('reverse',True)]:
  env=dict(os.environ,OPENTTD_TABLE_RANGE_SPLIT='1')
  if reverse:env['OPENTTD_TABLE_REVERSE_ORDER']='1'
  run([sys.executable,'-B',root/'utils/passenger_table_build_run.py',out/'openttd_test-timing.exe',out/label,
   '--fixture',build/'real-fixture','--skip-plan'],label+'-runner.log',env)
 (out/'completed.json').write_text(json.dumps(dict(success=True))+'\n')
finally:
 try:
  configure('','restore-configure.log');compile('restore-build.log')
  text=(build/'openttd_test.vcxproj').read_text(encoding='utf-8-sig')
  assert 'table-build' not in text
  sources={p.as_posix():hashlib.sha256((root/p).read_bytes()).hexdigest() for p in map(pathlib.Path,
   ['src/passenger_precomputed.cpp','utils/passenger_table_build_prepare.py','utils/passenger_table_build_benchmark.cpp'])}
  (out/'restored.json').write_text(json.dumps(dict(restored=True,sources=sources),indent=2)+'\n')
 finally:
  os.close(lock);lock_path.unlink()
