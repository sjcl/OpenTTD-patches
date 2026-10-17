import argparse,hashlib,json,os,pathlib,shutil,subprocess,sys,tempfile,xml.etree.ElementTree as ET
root=pathlib.Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'utils'))
import passenger_precomputed_real_map as real
base='902997e6314e018a08c4b92ec0083b040790005f';data=root/'build-msvc';parent=root/'build-reference-matched';source=parent/'source';build=parent/'build';parent.mkdir(exist_ok=True)
lock_path=pathlib.Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
print('Waiting for actual shared-lock handoff, with no time-based reservation.',flush=True)
lock=real.PROFILE.acquire_measurement_lock(lock_path,7200)
os.write(lock,json.dumps(dict(pid=os.getpid(),output=str(parent),purpose='fresh matching reference build and comparison')).encode())
def run(cmd,name):
 print('Running '+name,flush=True)
 with (parent/name).open('wb') as f:subprocess.run(list(map(str,cmd)),cwd=root,stdout=f,stderr=subprocess.STDOUT,check=True,creationflags=subprocess.CREATE_NO_WINDOW)
try:
 real.PROFILE.wait_for_idle(300,5)
 assert (parent/'source.json').exists()
 run(['cmake','-S',source,'-B',build,'-G','Visual Studio 17 2022','-A','x64','-DCMAKE_PREFIX_PATH=C:/Users/bebe/CLionProjects/OpenTTD-patches/cmake-build-release-visual-studio/vcpkg_installed/x64-windows-static','-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded','-DOPTION_DEDICATED=OFF','-DOPTION_USE_ASSERTS=ON','-DOPTION_NO_LTO=ON','-DOPTION_PASSENGER_BENCHMARK=OFF'],'configure.log')
 run([sys.executable,'-B',source/'utils/passenger_estimated_prepare.py',source,build,base,'--cpu'],'prepare-cpu.log')
 run(['cmake','--build',build,'--config','Release','--target','openttd','--parallel','12'],'build-cpu.log')
 ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'};xml=ET.parse(build/'openttd_lib.vcxproj')
 for g in xml.findall('m:ItemDefinitionGroup',ns):
  if "=='Release|x64'" in g.attrib.get('Condition',''):flags={n.tag.split('}')[-1]:n.text for n in g.find('m:ClCompile',ns)}
 expected=json.loads((root/'build-runtime-matched/cpu-provenance.json').read_text())['release_flags']
 differences={k:[expected.get(k),flags.get(k)] for k in expected.keys()|flags.keys() if k!='AdditionalIncludeDirectories' and expected.get(k)!=flags.get(k)}
 assert not differences,differences
 manifest=dict(base=base,source_index='source.json',compiler=real.PROFILE.compiler_info(build),release_flags=flags,candidate_flag_differences=differences,binary_sha256=hashlib.sha256((build/'Release/openttd.exe').read_bytes()).hexdigest(),copied_tus={str(p.relative_to(build)).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ['save-integrity','real-map-driver'] for p in (build/folder).glob('*') if p.is_file()})
 (parent/'cpu-provenance.json').write_text(json.dumps(manifest,indent=2)+'\n')
 for label,replacing in [('steady',False),('retained',True)]:
  print('Measuring fresh reference '+label,flush=True)
  args=argparse.Namespace(binary=build/'Release/openttd.exe',package=data/'package',output=data/('runtime-reference-fresh-'+label),input=data/'runtime-common-input/result.sav',lang=data/'lang',ticks=2048,timeout=900,resume=True,fixture_timetable=False,fixture_estimated=False,cold_precomputed=False,precomputed=True,warm_precomputed=not replacing,rebuild_precomputed=replacing,quality_probe=False,unpause_before_save=False,validation_only=False,affinity_mask=None,main_thread_affinity_mask=1)
  real.run(args,measurement_lock_fd=lock)
 real.summarize(argparse.Namespace(output=data/'runtime-reference-fresh-summary.json',runs=[data/'runtime-reference-fresh-steady',data/'runtime-reference-fresh-retained']))
 (parent/'completed.json').write_text(json.dumps(dict(success=True,base=base))+'\n')
 print('Fresh reference comparison completed.',flush=True)
finally:
 os.close(lock);lock_path.unlink()
