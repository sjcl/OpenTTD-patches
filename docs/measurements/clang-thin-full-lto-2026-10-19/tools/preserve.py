from pathlib import Path
import subprocess,json,hashlib,gzip,shutil,re
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'build-clang-lto-comparison'
OUT=ROOT/'docs/measurements/clang-thin-full-lto-2026-10-19'
OUT.mkdir(parents=True,exist_ok=True)
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def zipped(source,target):
    target.parent.mkdir(parents=True,exist_ok=True)
    with source.open('rb') as src,target.open('wb') as dst:
        with gzip.GzipFile(filename='',fileobj=dst,mode='wb',mtime=0) as stream:shutil.copyfileobj(src,stream)
for name in ('summary.json','host.json','recorder.log','cpu-sampled.wprp','compiler-version.txt'):
    shutil.copy2(BASE/name,OUT/name)
(OUT/'tools').mkdir(exist_ok=True)
for name in ('setup.py','build.py','measure.py','record.ps1','export.py','summarize.py','multiplayer.py','report.py','preserve.py','validate.py'):
    shutil.copy2(BASE/name,OUT/'tools'/name)
identity=dict(base_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),compiler='clang-cl / LLD 19.1.5',
    original_input=r'C:\Users\bebe\OneDrive\ドキュメント\OpenTTD\save\gc3833d807.sav',input_sha256=sha(BASE/'input/input.sav'),
    config=(BASE/'input/openttd.cfg').read_text(),source_files={},builds={})
for name in ('src/core/passenger_strategy.hpp','utils/passenger_precomputed_real_map.py','utils/passenger_precomputed_join.py'):
    identity['source_files'][name]=sha(ROOT/name)
for mode in ('thin','full'):
    build=BASE/mode
    cache=(build/'CMakeCache.txt').read_text()
    flags='\n'.join(line for line in cache.splitlines() if line.startswith(('CMAKE_CXX_FLAGS','CMAKE_EXE_LINKER_FLAGS:','CMAKE_MSVC_RUNTIME_LIBRARY:','CMAKE_GENERATOR:','CMAKE_GENERATOR_TOOLSET:','OPTION_USE_ASSERTS:','OPTION_NO_LTO:','OPTION_PASSENGER_BENCHMARK:')))
    rev=(build/'generated/rev.cpp').read_text()
    revision=re.search(r'const char _openttd_revision\[\] = "([^"]+)";',rev)[1]
    copied={p.relative_to(build).as_posix():sha(p) for folder in ('save-integrity','real-map-driver','cpu-measurement') for p in (build/folder).iterdir() if p.is_file()}
    objects={}
    for name in ('openttd.obj','passenger_precomputed.obj','passenger_service_database.obj'):
        p=build/'openttd_lib.dir/Release'/name
        with p.open('rb') as f:header=f.read(32)
        objects[name]=dict(sha256=sha(p),first_32_bytes=header.hex(),size=p.stat().st_size)
    identity['builds'][mode]=dict(exe_sha256=sha(build/'Release/openttd.exe'),pdb_sha256=sha(build/'Release/openttd.pdb'),language_sha256=sha(build/'lang/english.lng'),flags=flags,
        lto='-flto=thin' if mode=='thin' else '-flto',revision=revision,copied_tus=copied,objects=objects)
    for name in ('openttd.vcxproj','openttd_lib.vcxproj','measurement.cmake','configure-command.json','build-commands.json','driver-mode.txt','pdb-scope-check.txt','generated/rev.cpp'):
        zipped(build/name,OUT/'builds'/mode/(name+'.gz'))
    for p in (build/'openttd_lib.dir/Release').rglob('*.command.*.tlog'):zipped(p,OUT/'builds'/mode/(p.name+'.gz'))
    for folder in ('save-integrity','real-map-driver','cpu-measurement'):
        for p in (build/folder).iterdir():
            if p.is_file():zipped(p,OUT/'copied-tus'/mode/folder/(p.name+'.gz'))
    for prefix in ('configure','build'):zipped(BASE/f'{prefix}-{mode}.log',OUT/'checks'/f'{prefix}-{mode}.log.gz')
    shutil.copy2(build/'cpu-sampled.wpaProfile',OUT/'tools'/f'{mode}-cpu-sampled.wpaProfile')
accepted=json.loads((BASE/'summary.json').read_text())['runs']
for name in (r['name'] for r in accepted):
    source=BASE/'runs'/name;target=OUT/'runs'/name;target.mkdir(parents=True,exist_ok=True)
    for file in ('process.json','actual-ticks.json','save-integrity.json','trace-stats.txt','export-command.json','function-cpu.csv','wpr-ready.txt','wpr-finished.txt','wpr-go.txt','wpr-saved.txt'):
        if file=='trace-stats.txt':(target/file).write_text((source/file).read_text(encoding='utf-8-sig').rstrip('\r\n')+'\n',encoding='utf-8')
        else:shutil.copy2(source/file,target/file)
    for file in ('on-1-console.txt','stderr.txt','wpa-export.log'):zipped(source/file,target/(file+'.gz'))
    for p in (source/'wpa').glob('*.csv'):zipped(p,target/(p.name+'.gz'))
excluded=BASE/'dropped-events-thin-2'
zipped(excluded/'export.log',OUT/'checks'/'dropped-events-thin-2-export.log.gz')
shutil.copy2(excluded/'process.json',OUT/'checks'/'dropped-events-thin-2-process.json')
(OUT/'exclusions.json').write_text(json.dumps([dict(name='wpr-thin-2',reason='WPR dropped events',lost_events=1449719,retry='Administrator launch cancelled by Windows; no valid replacement run')],indent=2)+'\n')
mp=[]
for name in ('mp-thin-full','mp-full-thin'):
    source=BASE/name;target=OUT/name;target.mkdir(exist_ok=True)
    shutil.copy2(source/'result.json',target/'result.json');mp.append(json.loads((source/'result.json').read_text()))
    for peer in ('server','client'):
        for p in (source/peer).iterdir():
            if p.suffix in ('.json','.txt') and p.name!='openttd.cfg':zipped(p,target/peer/(p.name+'.gz'))
identity['multiplayer_complete']=all(r['pass'] for r in mp)
(OUT/'build-identity.json').write_text(json.dumps(identity,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print('Preserved',OUT)
