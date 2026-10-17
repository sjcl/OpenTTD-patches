"""Archive build preflight only; no synthetic WPR results or canonical claims."""
from pathlib import Path
import csv
import gzip
import hashlib
import json
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(__file__).resolve().parent
OUT = ROOT / 'docs/measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18'
CHECKS = OUT / 'checks'
CHECKS.mkdir(parents=True, exist_ok=True)


def sha(p):
    with p.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def dump(p, d):
    p.write_text(json.dumps(d, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def compress(source, target):
    with source.open('rb') as src, target.open('wb') as raw:
        with gzip.GzipFile(fileobj=raw, mode='wb', filename='', mtime=0) as dest:
            shutil.copyfileobj(src, dest)
    assert gzip.decompress(target.read_bytes()) == source.read_bytes()


for name in ['configure.log', 'build.log', 'build-retry.log', 'CMakeCache.txt']:
    compress(BUILD / name, CHECKS / (name + '.gz'))
for name in ['build-command.json', 'prepare_build.py', 'verify_native.py', 'measure.py',
             'native-all.log', 'rename-verification.json', 'input-search.json',
             'all-save-hashes-search.json', 'asset-recovery.json', 'package_preflight.py']:
    shutil.copy2(BUILD / name, CHECKS / name)
with (CHECKS / 'pdb-rsds.txt').open('wb') as f:
    subprocess.run(['C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/dumpbin.exe',
        '/headers', str(BUILD / 'Release/openttd.exe')], stdout=f, stderr=subprocess.STDOUT, check=True)
assert re.search(r'Passed all 729 test cases with 10074203 assertions', (BUILD / 'native-all.log').read_text())
guard = subprocess.run([sys.executable, '-B', str(BUILD / 'measure.py'), str(BUILD),
    '--legacy-package', str(ROOT / 'build-passenger-legacy-steady-8462e6cd/frozen-8192'),
    '--hyperpath-package', str(ROOT / 'build-passenger-steady-8462e6cd/frozen-8192'), '--check-inputs-only'],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(CHECKS / 'missing-input-guard.log').write_bytes(guard.stdout)
assert guard.returncode != 0 and b'FileNotFoundError' in guard.stdout
assert not list(BUILD.glob('*/cpu.etl')) and not list(BUILD.glob('*/wpr-ready.txt'))
files = {}
for name in ['Release/openttd.exe', 'Release/openttd.pdb', 'Release/openttd_test.exe', 'Release/openttd_test.pdb']:
    p = BUILD / name
    files[name] = dict(bytes=p.stat().st_size, sha256=sha(p))
identity = dict(base_commit='88cb61e3756cdd88021bc87b8dd779a4c93473d5',
    windows_build_fix_commit='b7cdf719e88350ca6b46a787a4fbd676de7f3352',
    product_src_tree='3f434b0f0ae350195bf77febfcee306d34a6b8f3',
    configured_on_base_then_built_with_identifier_rename=True,
    native_tests=dict(cases=729,assertions=10074203,exit_code=0,performance_measurement=False),
    files=files, build_flags=dict(cxx_release='/O2 /Ob2 /DNDEBUG /Zi /Zo',
        link='/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO',asserts=True,lto=False,static_crt=True),
    local_build=str(BUILD), copied_translation_units={}, tool_scripts={})
for directory in ['save-integrity', 'real-map-driver']:
    for p in sorted((BUILD / directory).glob('*')):
        if p.is_file():
            identity['copied_translation_units'][p.relative_to(BUILD).as_posix()] = sha(p)
            dest=CHECKS / directory / p.name; dest.parent.mkdir(exist_ok=True)
            shutil.copy2(p,dest)
for p in [ROOT / 'utils/passenger_precomputed_real_map.py', ROOT / 'utils/passenger_estimated_prepare.py',
          ROOT / 'utils/passenger_hyperpath_build.py', *sorted((ROOT / 'utils').glob('passenger_wpr_*'))]:
    if p.is_file(): identity['tool_scripts'][p.relative_to(ROOT).as_posix()] = sha(p)
subprocess.run(['git','diff','--quiet','b7cdf719e8','--','src'],cwd=ROOT,check=True)
dump(OUT / 'build-identity.json',identity)
reference=ROOT / 'docs/measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18'
targets=['openttd.exe!PassengerPrecomputedTable::StepHyperpath','openttd.exe!PassengerServiceDatabase::TickPrecomputed']
previous={}
for mode in ['legacy','hyperpath']:
    with (reference / (mode+'-functions.csv')).open(newline='',encoding='utf-8') as f:
        rows=list(csv.DictReader(f))
    previous[mode]={name:next((float(r['inclusive_ms_per_tick']) for r in rows if r['scope']=='simulation_stack' and r['function']==name),0.0) for name in targets}
dump(OUT / 'capture-status.json',dict(status='blocked_missing_inputs',wpr_recorded=False,
    measured_ticks=0,canonical_comparison=None,new_inclusive_cpu_ms_per_tick=None,
    reference='docs/measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18',
    previous_inclusive_cpu_ms_per_tick=previous,missing_input_sha256={
        'hyperpath':'275544f1fd7bb16920ebd325c3a916ee06f164fcbe53abf0c1bb6409e9802f9f',
        'legacy':'b276c5de400651f646c129ab394bc97a686f38aa345b3a4a799e07c8a06289ca',
        'original':'306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629'},
    assets_recovered=246,missing_input_guard=dict(exit_code=guard.returncode,wpr_started=False)))
(OUT / '.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n',encoding='utf-8')
dump(OUT / 'evidence-sha256.json',{p.relative_to(OUT).as_posix():dict(bytes=p.stat().st_size,sha256=sha(p)) for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='evidence-sha256.json'})
print('Archived build preflight; WPR and canonical comparison explicitly not performed')
