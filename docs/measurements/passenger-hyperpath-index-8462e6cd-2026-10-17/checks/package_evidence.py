from pathlib import Path
import gzip,hashlib,json,re,shutil,subprocess

root=Path('C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches')
build=root/'build-wpr-8462e6cd'
evidence=root/'docs/measurements/passenger-hyperpath-index-8462e6cd-2026-10-17'
baseline=root/'docs/measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17'
toolkit=root/'build-wpr-profile/tools/wpt/Windows Kits/10/Windows Performance Toolkit'

def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):
            digest.update(block)
    return digest.hexdigest()

def dump(path,value):
    path.write_text(json.dumps(value,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')

def compress(source,target):
    with source.open('rb') as src,target.open('wb') as dst:
        with gzip.GzipFile(filename='',fileobj=dst,mode='wb',mtime=0) as out:
            shutil.copyfileobj(src,out)

old=json.loads((baseline/'build-identity.json').read_text(encoding='utf-8'))
native=(build/'native-all.log').read_text(encoding='utf-8')
cases,assertions=map(int,re.search(r'Passed all ([0-9]+) test cases with ([0-9]+) assertions',native).groups())
assert cases==728 and assertions>0
subprocess.run(['git','diff','--quiet','8462e6cd','--','src'],cwd=root,check=True)
identity=dict(commit='8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4',
    checkout_commit='8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4',
    product_src_tree='631f498ec2e22d2910bb414d7f3b2f9b780b9e8e',
    product_sources_unchanged=True,
    baseline_commit='67cbbf32a48bac840d49037b2f9a90e8a1ca3aba',
    baseline_report_commit='b17a4e7a59d8a395e88fb7ca8b39409130519cd0',
    baseline_runtime_src_tree=old['product_src_tree'],
    native_tests=dict(cases=cases,assertions=assertions,exit_code=0),
    files={},copied_translation_units={},hardware=old['hardware'],
    pdb_rsds=re.search(r'Format: RSDS, \{([^}]+)\}, (\d+)',(build/'pdb-rsds.txt').read_text(encoding='utf-8',errors='replace')).groups(),
    build_flags=old['build_flags'],tool_scripts={},
    tools_version=old['tools_version'],tools={},symcache={},reference_files={})
for name in ['Release/openttd.exe','Release/openttd.pdb','Release/openttd_test.exe','Release/openttd_test.pdb','native-all.log','cpu-sampled.wpaProfile','build.log']:
    item=build/name
    identity['files'][name]=dict(bytes=item.stat().st_size,sha256=sha(item))
for directory in ['save-integrity','real-map-driver']:
    for item in sorted((build/directory).iterdir()):
        if item.suffix in ['.cpp','.h']:
            name=item.relative_to(build).as_posix()
            identity['copied_translation_units'][name]=sha(item)
            target=evidence/'copied-tus'/(name+'.gz')
            target.parent.mkdir(parents=True,exist_ok=True)
            compress(item,target)
for item in [root/'utils/passenger_precomputed_real_map.py',root/'utils/passenger_estimated_prepare.py',root/'utils/passenger_hyperpath_build.py',*sorted((root/'utils').glob('passenger_wpr_*'))]:
    if item.is_file():
        identity['tool_scripts'][item.relative_to(root).as_posix()]=sha(item)
for name,item in [('wpr.exe',Path('C:/Windows/System32/wpr.exe')),('wpaexporter.exe',toolkit/'wpaexporter.exe'),('perfcore.dll',toolkit/'perfcore.dll')]:
    identity['tools'][name]=sha(item)
assert identity['tools']==old['tools']
for item in (build/'symcache/openttd.pdb').rglob('*.symcache'):
    identity['symcache'][item.relative_to(build).as_posix()]=dict(bytes=item.stat().st_size,sha256=sha(item))
assert len(identity['symcache'])==1
for mode in ['legacy','hyperpath']:
    for name in ['save-integrity.json','CPU_Usage_(Sampled)_Stacks.csv.gz']:
        item=baseline/mode/name
        identity['reference_files'][item.relative_to(root).as_posix()]=sha(item)
for name in ['summary.json','build-identity.json','legacy-functions.csv','hyperpath-functions.csv']:
    item=baseline/name
    identity['reference_files'][item.relative_to(root).as_posix()]=sha(item)
for name in ['native-all.log','wpr-recorder.log','pdb-rsds.txt']:
    shutil.copy2(build/name,evidence/'checks'/name)
compress(build/'build.log',evidence/'checks/build.log.gz')
compress(root/'build-wpr-8462e6cd-configure.log',evidence/'checks/configure.log.gz')
compress(build/'CMakeCache.txt',evidence/'checks/CMakeCache.txt.gz')
shutil.copy2(build/'cpu-sampled.wpaProfile',evidence/'cpu-sampled.wpaProfile')
assert (build/'wpr-recorder-complete.txt').is_file()
shutil.copy2(build/'wpr-recorder-complete.txt',evidence/'checks/wpr-recorder-complete.txt')
for name in ['stages.json','maintenance.json']:
    item=root/'docs/measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17'/name
    identity['reference_files'][item.relative_to(root).as_posix()]=sha(item)
dump(evidence/'build-identity.json',identity)
print('Packaged build, tool, PDB and baseline provenance')
