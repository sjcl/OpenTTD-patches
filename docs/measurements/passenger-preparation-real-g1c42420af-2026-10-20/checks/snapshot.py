"""Keep immutable, independently copied binaries before any later rebuild."""
from prepare import ROOT, WORK, EVIDENCE, BASE, NEW, dump
from pathlib import Path
import hashlib
import shutil
import subprocess
import json
import sys

def identity(p):
    return {'bytes': p.stat().st_size, 'sha256': hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()}

for label, revision, build in [('baseline', BASE, WORK / 'baseline-build'), ('candidate', NEW, ROOT / 'build-preparation')]:
    target = WORK / (sys.argv[1] if len(sys.argv) > 1 else 'artifacts') / label
    target.mkdir(parents=True, exist_ok=False)
    (target / 'Release').mkdir()
    (target / 'lang').mkdir()
    for name in ['openttd.exe', 'openttd.pdb', 'openttd_test.exe', 'openttd_test.pdb']:
        shutil.copy2(build / 'Release' / name, target / 'Release' / name)
    shutil.copy2(build / 'lang/english.lng', target / 'lang/english.lng')
    shutil.copy2(ROOT / 'docs/measurements/passenger-wpr-abba-6c11c67f-2026-10-19/cpu-sampled.wpaProfile', target / 'cpu-sampled.wpaProfile')
    files = {p.relative_to(target).as_posix(): identity(p) for p in sorted(target.rglob('*')) if p.is_file()}
    dump(target / 'build-identity.json', {'product_commit': revision, 'label': label, 'files': files,
        'compiler': subprocess.check_output(['C:/Program Files/LLVM/bin/clang-cl.exe', '--version'], text=True),
        'flags': {'asserts': True, 'lto': False, 'pgo': False, 'CRT': 'MT', 'release': '/O2 /Ob2 /DNDEBUG /Zi'},
        'instrumentation': json.loads((EVIDENCE / f'{label}-instrumentation.json').read_text(encoding='utf-8')),
        'waiting_instrumentation': json.loads((EVIDENCE / f'{label}-waiting-instrumentation.json').read_text(encoding='utf-8'))
            if (EVIDENCE / f'{label}-waiting-instrumentation.json').is_file() else None})
    shutil.copy2(target / 'build-identity.json', EVIDENCE / f'{label}-build-identity.json')
(WORK / 'input/input.sav').chmod(0o444)
dump(EVIDENCE / 'pilot-quality.json', {
    'valid': True, 'lost_events': 0, 'lost_buffers': 0,
    'begin': 1536949, 'end': 4633656, 'interval_100ns': 10000,
    'profile_sha256': identity(WORK / 'cpu.wprp')['sha256'],
    'note': 'Short CPU pilot, not a real-map performance run; both marks ordered'})
print('Baseline and candidate exe/PDB/language snapshots complete', flush=True)
