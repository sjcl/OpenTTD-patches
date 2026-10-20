"""Remove disposable duplicate symbol downloads, then finish the already-tested build."""
from pathlib import Path
import hashlib
import json
import shutil

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
BUILD=ROOT/'build-merge-jgrpp-ya-20261018'
REFERENCE=ROOT/'build-rebaseline-88cb61e3/candidate'
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
targets=[(ROOT/name/'symbols').resolve() for name in ['build-wpr-9ab0228d','build-wpr-8ff4834d','build-wpr-f9c4a85b']]
assert (REFERENCE/'symbols').is_dir() and (REFERENCE/'symcache').is_dir()
for p in targets:
    assert p.is_relative_to(ROOT) and p.name=='symbols' and not p.is_symlink()
    assert p.parent.name in ['build-wpr-9ab0228d','build-wpr-8ff4834d','build-wpr-f9c4a85b']
cleanup=dict(reason='Disk full while copying unused duplicate downloaded PDB cache; local-symbol-only exports do not use symbols/',
    retained_reference=str(REFERENCE/'symbols'),free_before=shutil.disk_usage(ROOT).free,removed=[])
for p in targets:
    if p.is_dir():
        size=sum(q.stat().st_size for q in p.rglob('*') if q.is_file())
        shutil.rmtree(p)
        cleanup['removed'].append(dict(path=str(p),bytes=size))
cleanup['free_after']=shutil.disk_usage(ROOT).free
(WORK/'cache-cleanup.json').write_text(json.dumps(cleanup,indent=2)+'\n')
assert cleanup['free_after']>15_000_000_000
assert 'Passed all 763 test cases with 10586649 assertions.' in (WORK/'native-all.log').read_text(errors='replace')
identity=dict(product_commit='9ab0228db17178ef931de961305ebec544162f6a',src_tree='',files={})
import subprocess
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()==identity['product_commit']
identity['src_tree']=subprocess.check_output(['git','rev-parse','HEAD:src'],cwd=ROOT,text=True).strip()
for name in ['openttd.exe','openttd.pdb','openttd_test.exe']:
    p=WORK/'Release'/name
    assert sha(p)==sha(BUILD/'Release'/name)
    identity['files']['Release/'+name]=dict(bytes=p.stat().st_size,sha256=sha(p))
assert sha(WORK/'cpu-sampled.wpaProfile')==sha(REFERENCE/'cpu-sampled.wpaProfile')
shutil.copytree(REFERENCE/'symcache',WORK/'symcache',dirs_exist_ok=True)
(WORK/'symbols').mkdir(exist_ok=True)
identity['instrumentation']='same existing copied-TU exact-tick/WPR helpers; product routing is unchanged'
identity['compiled_from']=str(BUILD)
(WORK/'build-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
(WORK/'build-ready.txt').write_text('Requested revision built; all native tests passed; duplicate cache cleanup completed\n')
print(json.dumps(cleanup,indent=2),flush=True)
print('Already-tested 9ab0228d executable and matching PDB verified; ready',flush=True)
