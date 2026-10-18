from pathlib import Path
import subprocess, json, hashlib, gzip, shutil, csv
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
BASE = ROOT / 'build-compiler-lto-comparison'
OUT = ROOT / 'docs/measurements/compiler-lto-real-map-2026-10-19'
OUT.mkdir(parents=True, exist_ok=True)
def sha(path):
    with path.open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()
def zipped(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    with source.open('rb') as src, target.open('wb') as dst:
        with gzip.GzipFile(filename='', fileobj=dst, mode='wb', mtime=0) as stream: shutil.copyfileobj(src, stream)
for name in ('summary.json', 'host.json', 'recorder.log', 'cpu-sampled.wprp', 'lto-object-checks.json'):
    shutil.copy2(BASE/name, OUT/name)
tools = OUT / 'tools'
tools.mkdir(exist_ok=True)
for name in ('setup.py', 'build.py', 'acquire_msvc.py', 'measure.py', 'record.ps1', 'export.py', 'summarize.py', 'multiplayer.py', 'preserve.py', 'validate.py'):
    shutil.copy2(BASE/name, tools/name)
identity = dict(base_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
    product_patch=subprocess.check_output(['git','diff','--','src/core/passenger_strategy.hpp'],cwd=ROOT,text=True),
    input_sha256=sha(BASE/'input/input.sav'), original_input=r'C:\Users\bebe\OneDrive\ドキュメント\OpenTTD\save\gc3833d807.sav',
    config=(BASE/'input/openttd.cfg').read_text(), source_files={}, builds={})
identity['isolated_msvc_acquisition'] = json.loads((BASE/'toolchain-new/acquisition.json').read_text())
identity['isolated_msvc_files'] = {p.relative_to(BASE/'toolchain-new/extracted').as_posix():sha(p) for p in (BASE/'toolchain-new/extracted').rglob('*') if p.is_file()}
identity['compiler_versions'] = dict(msvc='19.51.36260.0', clang='19.1.5', msvc_stl='14.44.35207')
for name in ('src/core/passenger_strategy.hpp', 'utils/passenger_precomputed_real_map.py', 'utils/passenger_precomputed_join.py'):
    identity['source_files'][name]=sha(ROOT/name)
for compiler in ('msvc','clang'):
    build = BASE/compiler
    cache = (build/'CMakeCache.txt').read_text()
    relevant = '\n'.join(line for line in cache.splitlines() if line.startswith(('CMAKE_CXX_FLAGS', 'CMAKE_EXE_LINKER_FLAGS:', 'CMAKE_MSVC_RUNTIME_LIBRARY:', 'CMAKE_GENERATOR:', 'CMAKE_GENERATOR_TOOLSET:', 'OPTION_USE_ASSERTS:', 'OPTION_NO_LTO:', 'OPTION_PASSENGER_BENCHMARK:')))
    copied = {p.relative_to(build).as_posix():sha(p) for folder in ('save-integrity','real-map-driver','cpu-measurement') for p in (build/folder).glob('*') if p.is_file()}
    identity['builds'][compiler] = dict(exe_sha256=sha(build/'Release/openttd.exe'), pdb_sha256=sha(build/'Release/openttd.pdb'), language_sha256=sha(build/'lang/english.lng'), flags=relevant, copied_tus=copied)
    identity['builds'][compiler]['lto'] = 'MSVC /GL + /LTCG' if compiler == 'msvc' else 'clang-cl -flto=thin + LLD'
    for name in ('openttd.vcxproj', 'openttd_lib.vcxproj', 'measurement.cmake', 'configure-command.json', 'build-commands.json'):
        zipped(build/name, OUT/'builds'/compiler/(name+'.gz'))
    for p in (build/'openttd_lib.dir/Release').rglob('*.command.*.tlog'):
        zipped(p, OUT/'builds'/compiler/(p.name+'.gz'))
    for folder in ('save-integrity','real-map-driver','cpu-measurement'):
        for source in (build/folder).glob('*'):
            if source.is_file(): zipped(source, OUT/'copied-tus'/compiler/folder/(source.name+'.gz'))
    for suffix in ('retry.log', 'parallel.log', 'cgthreads.log', 'zoning.log', 'public-pdb.log', 'notypeopt.log', 'serviced.log', '1951-objects.log', '1951-lib.log', '1951.log', '1951-final.log', 'log'):
        p = BASE/(f'build-{compiler}.log' if suffix == 'log' else f'build-{compiler}-{suffix}')
        if p.exists(): zipped(p, OUT/'checks'/(p.name+'.gz'))
    shutil.copy2(build/'cpu-sampled.wpaProfile', tools/f'{compiler}-cpu-sampled.wpaProfile')
for name in ('wpr-msvc-1','wpr-clang-1','wpr-clang-2','wpr-msvc-2'):
    source = BASE/'runs'/name
    target = OUT/'runs'/name
    target.mkdir(parents=True, exist_ok=True)
    for file in ('process.json','actual-ticks.json','save-integrity.json','trace-stats.txt','export-command.json','function-cpu.csv','wpr-ready.txt','wpr-finished.txt','wpr-go.txt','wpr-saved.txt'):
        if file == 'trace-stats.txt':
            (target/file).write_text((source/file).read_text(encoding='utf-8-sig').rstrip('\r\n')+'\n',encoding='utf-8')
        else:
            shutil.copy2(source/file,target/file)
    for file in ('on-1-console.txt','stderr.txt','wpa-export.log'):
        zipped(source/file,target/(file+'.gz'))
    for p in (source/'wpa').glob('*.csv'):
        zipped(p,target/(p.name+'.gz'))
mp = {}
for name in ('mp-msvc-clang','mp-clang-msvc'):
    source = BASE/name
    if not (source/'result.json').exists(): continue
    target = OUT/name
    target.mkdir(exist_ok=True)
    shutil.copy2(source/'result.json',target/'result.json')
    mp[name]=json.loads((source/'result.json').read_text())
    for peer in ('server','client'):
        for p in (source/peer).glob('*'):
            if p.suffix in ('.json','.txt') and p.name != 'openttd.cfg': zipped(p,target/peer/(p.name+'.gz'))
identity['multiplayer_complete'] = len(mp)==2 and all(r['pass'] for r in mp.values())
(OUT/'build-identity.json').write_text(json.dumps(identity,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print('Preserved',OUT)
