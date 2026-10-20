"""Freeze the unchanged gc3833d807 input and verify every asset against 97cdc0ad."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import stat
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OLD=ROOT/'build-wpr-97cdc0ad'
BASE=ROOT/'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
SOURCE=Path('C:/Users/bebe/OneDrive/ドキュメント/OpenTTD/save/gc3833d807.sav')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def link_or_copy(source,dest):
    if Path(source).name in ['input.sav','openttd.cfg','package.json']:return shutil.copy2(source,dest)
    try:os.link(source,dest)
    except OSError:shutil.copy2(source,dest)
    return str(dest)
old=load(BASE/'input-identity.json');manifest=load(BASE/'input/package.json')
assert sha(SOURCE)==sha(OLD/'input/input.sav')==old['input_sha256']==manifest['input_sha256']
assert sha(OLD/'input/package.json')==sha(BASE/'input/package.json')
shutil.copytree(OLD/'input',WORK/'input',copy_function=link_or_copy)
assert sha(WORK/'input/input.sav')==old['input_sha256']
assert sha(WORK/'input/openttd.cfg')==manifest['config_sha256']
assert all(sha(WORK/'input'/e['path'])==e['sha256'] for e in manifest['assets'])
(WORK/'input/input.sav').chmod(stat.S_IREAD)
result=dict(source=str(SOURCE),source_bytes=SOURCE.stat().st_size,input_sha256=old['input_sha256'],
    copied_without_conversion=True,same_as_97cdc0ad=True,config_sha256=manifest['config_sha256'],assets_verified=len(manifest['assets']),
    start_tick=old['start_tick'],target_end_tick=old['target_end_tick'],requested_ticks=2048,
    baseline_input_validation_sha256=sha(BASE/'input-identity.json'),baseline_package_manifest_sha256=sha(BASE/'input/package.json'),
    note='Same frozen input directly in both modes; no additional warmup ticks or derived saves')
(WORK/'input-identity.json').write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2,ensure_ascii=False),flush=True)
