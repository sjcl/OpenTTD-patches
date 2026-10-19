from pathlib import Path
import subprocess,json,gzip,shutil,hashlib,re
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'build-clang-thin-pgo'
OUT=ROOT/'docs/measurements/clang-thin-pgo-2026-10-19'
OUT.mkdir(parents=True,exist_ok=True)
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def zipped(source,target):
    target.parent.mkdir(parents=True,exist_ok=True)
    with source.open('rb') as src,target.open('wb') as dst:
        with gzip.GzipFile(filename='',fileobj=dst,mode='wb',mtime=0) as stream:shutil.copyfileobj(src,stream)
def copy(source,target):
    target.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(source,target)
sampled=(BASE/'summary.json').exists()
for name in ('summary-plain.json','host.json','option-checks.json','profile-use-checks.json','profile-invalidation.json','cpu-sampled.wprp','measurement.cmake','versions.txt','admin-launch.json'):
    copy(BASE/name,OUT/name)
if sampled:copy(BASE/'summary.json',OUT/'summary.json')
if (BASE/'recorder.log').exists():
    (OUT/'recorder.log').write_text((BASE/'recorder.log').read_text(encoding='utf-8-sig').rstrip()+'\n',encoding='utf-8')
for name in ('setup.py','build.py','train.py','check-options.py','check-options.cmake','check-invalidation.py','check-profile.py','measure.py','record.ps1','export.py','summarize.py','summarize-plain.py','multiplayer.py','preserve.py','validate.py','report.py'):
    copy(BASE/name,OUT/'tools'/name)
copy(BASE/'report-template.md',OUT/'tools'/'report-template.md')
for name in ('test-build.log','tests.log','check-profile.log','check-invalidation.log'):
    zipped(BASE/name,OUT/'checks'/(name+'.gz'))
for name in ('configure.log','build-first.log','build-updated.log'):
    zipped(BASE/'invalidation'/name,OUT/'checks'/'invalidation'/(name+'.gz'))
for file in (BASE/'profiles').iterdir():
    if file.is_file():
        if file.suffix=='.profdata':zipped(file,OUT/'profiles'/(file.name+'.gz'))
        else:copy(file,OUT/'profiles'/file.name)
for folder in ('save-integrity','real-map-driver','cpu-measurement'):
    for file in (BASE/'measurement'/folder).iterdir():
        if file.is_file():zipped(file,OUT/'copied-tus'/folder/(file.name+'.gz'))
# Freeze the compiled source base: documentation may be committed after builds.
identity=dict(base_commit='246bbdbf8f0b59e524d40b7b0b104f5a965c7858',compiler='clang-cl/LLD/llvm-profdata 19.1.5',
    input_sha256=sha(BASE/'input/input.sav'),original_input=r'C:\Users\bebe\OneDrive\ドキュメント\OpenTTD\save\gc3833d807.sav',
    profile_sha256=sha(BASE/'profiles/real-map.profdata'),source_files={},builds={},sampled_cpu_available=sampled,
    test_binary_sha256=sha(BASE/'pgo/Release/openttd_test.exe'))
for name in ('CMakeLists.txt','cmake/ProfileGuidedOptimization.cmake','utils/passenger_precomputed_real_map.py','utils/passenger_precomputed_join.py'):
    identity['source_files'][name]=sha(ROOT/name)
for mode in ('baseline','generate','pgo'):
    build=BASE/mode
    revision=re.search(r'const char _openttd_revision\[\] = "([^"]+)";',(build/'generated/rev.cpp').read_text())[1]
    cache=(build/'CMakeCache.txt').read_text()
    flags='\n'.join(line for line in cache.splitlines() if line.startswith(('OPTION_PGO:','PGO_PROFILE_FILE:','CMAKE_CXX_FLAGS','CMAKE_EXE_LINKER_FLAGS:','CMAKE_MSVC_RUNTIME_LIBRARY:','OPTION_USE_ASSERTS:','OPTION_PASSENGER_BENCHMARK:')))
    identity['builds'][mode]=dict(exe_sha256=sha(build/'Release/openttd.exe'),pdb_sha256=sha(build/'Release/openttd.pdb'),revision=revision,flags=flags)
    for name in ('configure-command.json','build-commands.json','openttd.vcxproj','openttd_lib.vcxproj','generated/rev.cpp'):
        zipped(build/name,OUT/'builds'/mode/(name+'.gz'))
    for file in (build/'openttd_lib.dir/Release').rglob('*.command.*.tlog'):zipped(file,OUT/'builds'/mode/(file.name+'.gz'))
    for prefix in ('configure','build'):zipped(BASE/f'{prefix}-{mode}.log',OUT/'checks'/f'{prefix}-{mode}.log.gz')
    copy(build/'cpu-sampled.wpaProfile',OUT/'tools'/f'{mode}-cpu-sampled.wpaProfile')
names=['training','runs/plain-baseline-1','runs/plain-pgo-1']
if sampled:names+=['runs/wpr-baseline-1','runs/wpr-pgo-1']
for name in names:
    source=BASE/name;target=OUT/name
    for file in ('process.json','actual-ticks.json','save-integrity.json'):
        copy(source/file,target/file)
    for file in ('on-1-console.txt','stderr.txt'):zipped(source/file,target/(file+'.gz'))
    if name.startswith('runs/wpr-'):
        for file in ('export-command.json','function-cpu.csv','wpr-ready.txt','wpr-go.txt','wpr-finished.txt','wpr-saved.txt'):copy(source/file,target/file)
        (target/'trace-stats.txt').write_text((source/'trace-stats.txt').read_text(encoding='utf-8-sig').rstrip()+'\n',encoding='utf-8')
        for file in (source/'wpa').glob('*.csv'):zipped(file,target/(file.name+'.gz'))
        zipped(source/'wpa-export.log',target/'wpa-export.log.gz')
    if name!='training':copy(BASE/(source.name+'-command.json'),target/'run-command.json')
for name in ('mp-baseline-pgo','mp-pgo-baseline'):
    source=BASE/name;target=OUT/name
    copy(source/'result.json',target/'result.json')
    for peer in ('server','client'):
        for file in (source/peer).iterdir():
            if file.suffix in ('.json','.txt') and file.name!='openttd.cfg':zipped(file,target/peer/(file.name+'.gz'))
(OUT/'build-identity.json').write_text(json.dumps(identity,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('Preserved',OUT)
