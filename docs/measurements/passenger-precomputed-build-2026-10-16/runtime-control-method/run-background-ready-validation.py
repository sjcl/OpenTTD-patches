"""Compare exact base/current builds after loaded linkgraph jobs have finished.

Prepare only until the peer's next real lease has been observed, then launch.
The console wait reads completion flags; it does not merge or publish jobs.
"""
import argparse,hashlib,json,os,pathlib,shutil,subprocess,sys,tempfile
import xml.etree.ElementTree as ET
root=pathlib.Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'utils'))
import passenger_precomputed_real_map as real
data=root/'build-msvc';out=data/'background-ready-control';out.mkdir(exist_ok=True)
reference=root/'build-reference-matched';source_old=reference/'source';build_old=reference/'build'
build_new=root/'build-runtime-matched'
lock_path=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
print('Waiting for actual shared-lock handoff.',flush=True)
lock=real.PROFILE.acquire_measurement_lock(lock_path,14400)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(out),purpose='base/current tick comparison with loaded background jobs aligned')).encode())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
projects=[('old',source_old,build_old),('new',root,build_new)]
configured=[]

def run(cmd,log):
 print('Running '+log,flush=True)
 with (out/log).open('wb') as stream:
  subprocess.run(list(map(str,cmd)),cwd=root,stdout=stream,stderr=subprocess.STDOUT,
                 check=True,creationflags=subprocess.CREATE_NO_WINDOW)

def flags(build):
 ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
 for g in ET.parse(build/'openttd_lib.vcxproj').findall('m:ItemDefinitionGroup',ns):
  if "=='Release|x64'" in g.attrib.get('Condition',''):
   return {n.tag.split('}')[-1]:n.text for n in g.find('m:ClCompile',ns)}
 raise ValueError('Missing Release compiler flags')

try:
 real.PROFILE.wait_for_idle(300,5)
 originals=json.loads((reference/'source.json').read_text())['products']
 assert all(sha(source_old/name)==digest for name,digest in originals.items())
 expected={'passenger_precomputed.cpp':'d89fb2a43a0614a9255addcd31a9cdd69cf47e82d5aceec5fd06ba246883aaec',
           'passenger_precomputed.h':'8e51935b3f5855edca482da3c887bcba34bde7e83fdee8c95a6edd54123d4bd4'}
 assert all(sha(root/'src'/name)==digest for name,digest in expected.items())
 # Only diagnostics are copied into the archive; the 1866 product files stay exact.
 for path in (root/'utils').glob('passenger_*.py'):shutil.copy2(path,source_old/'utils'/path.name)
 revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
 provenance={}
 for label,source,build in projects:
  run(['cmake','-S',source,'-B',build,'-DCMAKE_PROJECT_INCLUDE='],label+'-configure.log')
  configured.append((label,source,build))
  run([sys.executable,'-B',source/'utils/passenger_estimated_prepare.py',source,build,
       '902997e6314e018a08c4b92ec0083b040790005f' if label=='old' else revision,'--cpu'],label+'-prepare.log')
  run(['cmake','--build',build,'--config','Release','--target','openttd','--parallel','12'],label+'-build.log')
  binary=out/(label+'-binary');binary.mkdir(exist_ok=True);shutil.copy2(build/'Release/openttd.exe',binary/'openttd.exe')
  provenance[label]=dict(source_head='902997e6314e018a08c4b92ec0083b040790005f' if label=='old' else revision,
   release_flags=flags(build),compiler=real.PROFILE.compiler_info(build),binary_sha256=sha(binary/'openttd.exe'),
   copied_tus={str(p.relative_to(build)):sha(p) for folder in ['save-integrity','real-map-driver'] for p in (build/folder).glob('*') if p.is_file()})
 differences={k:[provenance['old']['release_flags'].get(k),provenance['new']['release_flags'].get(k)]
              for k in provenance['old']['release_flags'].keys()|provenance['new']['release_flags'].keys()
              if k!='AdditionalIncludeDirectories' and provenance['old']['release_flags'].get(k)!=provenance['new']['release_flags'].get(k)}
 assert not differences,differences
 provenance['flag_differences']=differences
 provenance['new_product_sha256']={n:sha(root/n) for n in ['src/passenger_precomputed.cpp','src/passenger_precomputed.h',
  'src/passenger_service_database.cpp','src/passenger_service_database.h','src/sl/extended_ver_sl.cpp']}
 provenance['old_product_files_verified']=len(originals)
 provenance['tools']={p.name:sha(p) for p in (root/'utils').glob('passenger_*.py')}
 (out/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
 captured=out/'measurement-sources';captured.mkdir(exist_ok=True)
 for name in ['passenger_precomputed_real_map.py','passenger_precomputed_join_prepare.py','passenger_estimated_prepare.py','passenger_host_observer.py']:
  shutil.copy2(root/'utils'/name,captured/name)
 checks={};runs=[]
 for label in ['old-a','new-a','new-b','old-b']:
  version=label.split('-')[0];target=data/('runtime-background-ready-'+label)
  print('Measuring '+label,flush=True)
  args=argparse.Namespace(binary=out/(version+'-binary')/'openttd.exe',package=data/'package',output=target,
   input=data/'runtime-common-input/result.sav',lang=data/'lang',ticks=2048,timeout=900,resume=True,
   fixture_timetable=False,fixture_estimated=False,cold_precomputed=False,precomputed=True,warm_precomputed=True,
   rebuild_precomputed=False,quality_probe=False,unpause_before_save=False,validation_only=False,
   affinity_mask=None,main_thread_affinity_mask=1,observe_host=True,wait_background_ready=True)
  real.run(args,measurement_lock_fd=lock);runs.append(target)
  baseline='runtime-reference-fresh-steady' if version=='old' else 'runtime-main-reverse-new'
  old=json.loads((data/baseline/'save-integrity.json').read_text());new=json.loads((target/'save-integrity.json').read_text())
  mismatch={k:[old.get(k),new.get(k)] for k in old.keys()|new.keys() if old.get(k)!=new.get(k)}
  checks[label]=dict(reference=baseline,mismatches=mismatch)
  (out/'canonical.json').write_text(json.dumps(checks,indent=2)+'\n');assert not mismatch,mismatch
 real.summarize(argparse.Namespace(output=data/'runtime-background-ready-summary.json',runs=runs))
 (out/'completed.json').write_text(json.dumps(dict(success=True,product_changed=False))+'\n')
finally:
 errors=[]
 for label,source,build in configured:
  try:
   run(['cmake','-S',source,'-B',build,'-DCMAKE_PROJECT_INCLUDE='],label+'-restore-configure.log')
   run(['cmake','--build',build,'--config','Release','--target','openttd','--parallel','12'],label+'-restore-build.log')
   text=(build/'openttd_lib.vcxproj').read_text(encoding='utf-8-sig')
   assert not any(x in text for x in ['save-integrity','real-map-driver','table-build'])
  except Exception as error:errors.append(str(error))
 os.close(lock);lock_path.unlink()
 if errors:raise RuntimeError('Product restoration failed: '+str(errors))
