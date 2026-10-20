"""Build exactly 0b8a21c0 with the prior MSVC WPR flags and copied measurement TUs."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
BUILD=ROOT/'build-wpr-97cdc0ad/compile'
OLD=ROOT/'build-wpr-97cdc0ad'
BASE=ROOT/'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
REVISION='0b8a21c0f3dc9a24ca0d9d3581c19fb9c207e684'
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def hardlink_or_copy(source,dest):
    try:os.link(source,dest)
    except OSError:shutil.copy2(source,dest)
    return str(dest)
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()==REVISION
assert not subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip()
env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
config=load(BASE/'build-command.json')
config['configure_command'][2]=str(ROOT);config['configure_command'][4]=str(BUILD)
config['checkout_commit']=config['measurement_target']=REVISION
config['native_test_command'][0]=str(BUILD/'Release/openttd_test.exe')
(WORK/'build-command.json').write_text(json.dumps(config,indent=2)+'\n')
with (WORK/'configure.log').open('wb') as out:
    subprocess.run(config['configure_command'],cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
for tool,args in [('passenger_estimated_prepare.py',[str(ROOT),str(BUILD),REVISION,'--cpu']),('passenger_wpr_prepare.py',[str(BUILD)])]:
    script=WORK/'estimated_prepare_compat.py' if tool=='passenger_estimated_prepare.py' else ROOT/'utils'/tool
    subprocess.run([sys.executable,'-B',str(script),*args],cwd=ROOT,env=env,check=True)
with (WORK/'build.log').open('wb') as out:
    subprocess.run(['cmake','--build',str(BUILD),'--config','Release','--target','openttd_test','openttd','--parallel','12','--','/nodeReuse:false'],cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
with (WORK/'native-all.log').open('wb') as out:
    subprocess.run(config['native_test_command'],cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
print((WORK/'native-all.log').read_text(errors='replace')[-400:],flush=True)
identity=dict(product_commit=REVISION,src_tree=subprocess.check_output(['git','rev-parse','HEAD:src'],cwd=ROOT,text=True).strip(),files={})
(WORK/'Release').mkdir(exist_ok=True)
for name in ['openttd.exe','openttd.pdb','openttd_test.exe']:
    source=BUILD/'Release'/name;dest=WORK/'Release'/name;shutil.copy2(source,dest)
    assert sha(source)==sha(dest)
    identity['files']['Release/'+name]=dict(bytes=dest.stat().st_size,sha256=sha(dest))
for folder in ['lang','baseset','save-integrity','real-map-driver']:
    shutil.copytree(BUILD/folder,WORK/folder)
shutil.copy2(OLD/'cpu-sampled.wpaProfile',WORK/'cpu-sampled.wpaProfile')
shutil.copytree(OLD/'symcache',WORK/'symcache',copy_function=hardlink_or_copy)
identity.update(instrumentation='same copied-TU exact-tick/WPR helpers as 97cdc0ad; no product source edits',compiled_from=str(BUILD))
(WORK/'build-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
(WORK/'build-ready.txt').write_text('Requested revision built and native tests passed\n')
print('0b8a21c0 executable/PDB prepared',flush=True)
