"""Reuse the exact f9c4a85b input, config and assets; never evolve or convert the save."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import stat

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OLD=ROOT/'build-wpr-f9c4a85b'
SOURCE=Path('C:/Users/bebe/OneDrive/ドキュメント/OpenTTD/save/gc3833d807.sav')
EXPECTED='916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a'
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def link_or_copy(source,dest):
    if Path(source).name in ['input.sav','openttd.cfg','package.json']:return shutil.copy2(source,dest)
    try:os.link(source,dest)
    except OSError:shutil.copy2(source,dest)
    return str(dest)
baseline=load(OLD/'input-validation.json');manifest=load(OLD/'input/package.json')
assert sha(SOURCE)==sha(OLD/'input/input.sav')==manifest['input_sha256']==EXPECTED
shutil.copytree(OLD/'input',WORK/'input',copy_function=link_or_copy)
assert sha(WORK/'input/input.sav')==EXPECTED
assert sha(WORK/'input/openttd.cfg')==manifest['config_sha256']
assert sha(WORK/'input/package.json')==sha(OLD/'input/package.json')
assert all(sha(WORK/'input'/e['path'])==e['sha256'] for e in manifest['assets'])
(WORK/'input/input.sav').chmod(stat.S_IREAD)
result=dict(source=str(SOURCE),source_bytes=SOURCE.stat().st_size,input_sha256=EXPECTED,
    frozen_input_sha256=EXPECTED,copied_without_conversion=True,same_as_f9c4a85b=True,
    config_sha256=manifest['config_sha256'],assets_verified=len(manifest['assets']),
    start_tick=baseline['start_tick'],target_end_tick=baseline['target_end_tick'],requested_ticks=2048,
    baseline_input_validation_sha256=sha(OLD/'input-validation.json'),
    baseline_package_manifest_sha256=sha(OLD/'input/package.json'),
    note='The original frozen input is used directly for both modes; no extra warmup ticks or derived input saves')
(WORK/'input-identity.json').write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2,ensure_ascii=False))
