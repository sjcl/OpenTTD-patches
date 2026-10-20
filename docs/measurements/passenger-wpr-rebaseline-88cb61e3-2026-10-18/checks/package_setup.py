"""Archive replacement-input provenance and immutable old/new build identities."""
from pathlib import Path
import gzip
import hashlib
import json
import shutil

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OUT=ROOT/'docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18'
CHECKS=OUT/'checks'; CHECKS.mkdir(parents=True,exist_ok=True)

def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def dump(p,d):p.write_text(json.dumps(d,indent=2)+'\n',encoding='utf-8')
def compress(p,q):
    q.parent.mkdir(parents=True,exist_ok=True)
    with p.open('rb') as src,q.open('wb') as raw:
        with gzip.GzipFile(fileobj=raw,mode='wb',filename='',mtime=0) as dest:shutil.copyfileobj(src,dest)
def copy(p,q):q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)

(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/trace-stats.txt -whitespace\n**/window-markers.txt -whitespace\n',encoding='utf-8')
for p in WORK.glob('*.py'):copy(p,CHECKS/p.name)
for p in WORK.glob('*.ps1'):copy(p,CHECKS/p.name)
for name in ['seed-validation.json','seeds-complete.json','input-backup.json','hardware.json','power-policy.txt']:
    copy(WORK/name,OUT/name)
for package in ['original','seeds/legacy','seeds/hyperpath']:
    for name in ['package.json','openttd.cfg']:copy(WORK/package/name,OUT/package/name)
for stage in ['warm-legacy','warm-hyperpath','reload-legacy','reload-hyperpath','input-probe-save2']:
    for p in (WORK/stage).glob('*'):
        if not p.is_file():continue
        if p.name in ['save-integrity.json','transport-checkpoint.json']:copy(p,OUT/'preparation'/stage/p.name)
        elif p.suffix in ['.json','.txt']:compress(p,OUT/'preparation'/stage/(p.name+'.gz'))
compress(WORK/'input-probe/stderr.txt',CHECKS/'rejected-ga58-load-stderr.txt.gz')
old=WORK/'baseline';new=WORK/'candidate'
for name in ['configure.log','build.log','CMakeCache.txt']:compress(old/name,CHECKS/'baseline'/(name+'.gz'))
for name in ['build-command.json','native-all.log','source-verification.json','idle-worker-cleanup.json']:
    copy(old/name,CHECKS/'baseline'/name)
for folder in ['save-integrity','real-map-driver']:
    for p in (old/folder).glob('*'):
        if p.is_file():copy(p,CHECKS/'baseline'/folder/p.name)
previous=ROOT/'docs/measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18'
copy(previous/'build-identity.json',CHECKS/'candidate-preflight-build-identity.json')
copy(previous/'checks/native-all.log',CHECKS/'candidate-native-all.log')
copy(previous/'checks/rename-verification.json',CHECKS/'candidate-rename-verification.json')
identities={}
for version,build,commit,tree in [
    ('baseline',old,'8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4','631f498ec2e22d2910bb414d7f3b2f9b780b9e8e'),
    ('candidate',new,'88cb61e3756cdd88021bc87b8dd779a4c93473d5','3f434b0f0ae350195bf77febfcee306d34a6b8f3')]:
    files={}
    for name in ['Release/openttd.exe','Release/openttd.pdb']:
        p=build/name;files[name]=dict(bytes=p.stat().st_size,sha256=sha(p))
    identities[version]=dict(product_commit=commit,src_tree=tree,files=files,build_flags=dict(
        cxx_release='/O2 /Ob2 /DNDEBUG /Zi /Zo',c_release='/O2 /Ob2 /DNDEBUG /Zi',
        link='/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO',static_crt=True,asserts=True,lto=False),
        local_build=str(build))
identities['baseline'].update(native_tests=dict(cases=728,assertions=9930179,exit_code=0),
    source_archive_sha256=sha(WORK/'source-8462e6cd.tar'),source_archive_windows_line_endings=True,
    revision_metadata_note='Source archive retains its committed .ottdrev-vc release label; src contents are independently verified against 8462e6cd. The copied measurement driver explicitly identifies 8462e6cd.')
identities['candidate'].update(windows_identifier_fix_commit='b7cdf719e88350ca6b46a787a4fbd676de7f3352',
    native_tests=dict(cases=729,assertions=10074203,exit_code=0),reused_verified_preflight_binary=True)
prior=json.loads((previous/'build-identity.json').read_text())
for name,d in identities['candidate']['files'].items():assert d==prior['files'][name]
toolkit=WORK/'tools/wpt/Windows Kits/10/Windows Performance Toolkit'
identities['tools']={name:dict(bytes=p.stat().st_size,sha256=sha(p)) for name,p in [
    ('wpr.exe',Path('C:/Windows/System32/wpr.exe')),('wpaexporter.exe',toolkit/'wpaexporter.exe'),
    ('xperf.exe',toolkit/'xperf.exe'),('perfcore.dll',toolkit/'perfcore.dll')]}
identities['tool_source']='https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install'
identities['wpaexporter_version']='11.7.395.48728'
dump(OUT/'build-identities.json',identities)
print('Replacement-input and build provenance archived',flush=True)
