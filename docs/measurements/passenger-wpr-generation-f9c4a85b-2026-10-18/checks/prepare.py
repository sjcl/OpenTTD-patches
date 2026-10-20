"""Build the requested revision with the same copied-TU WPR instrumentation."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
BUILD=ROOT/'build-merge-jgrpp-ya-20261018'
REVISION='f9c4a85b721073bbf86747f4a4cec7ac5ccd2f89'
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()==REVISION
assert not subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip()
env=dict(os.environ)
for key in list(env):
    if key.startswith('OPENTTD_'):env.pop(key)
config=json.loads((ROOT/'build-wpr-88cb61e3/build-command.json').read_text())
config['configure_command'][2]=str(ROOT)
config['configure_command'][4]=str(BUILD)
config['checkout_commit']=config['measurement_target']=REVISION
config.pop('windows_build_fix_commit',None)
config['native_test_command'][0]=str(BUILD/'Release/openttd_test.exe')
(WORK/'build-command.json').write_text(json.dumps(config,indent=2)+'\n')
with (WORK/'configure.log').open('wb') as out:
    subprocess.run(config['configure_command'],cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
for tool,arguments in [
    ('passenger_estimated_prepare.py',[str(ROOT),str(BUILD),REVISION,'--cpu']),
    ('passenger_wpr_prepare.py',[str(BUILD)])]:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils'/tool),*arguments],cwd=ROOT,env=env,check=True)
with (WORK/'build.log').open('wb') as out:
    subprocess.run(['cmake','--build',str(BUILD),'--config','Release','--target','openttd_test','openttd','--parallel','12','--','/nodeReuse:false'],cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
with (WORK/'native-all.log').open('wb') as out:
    subprocess.run(config['native_test_command'],cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
print((WORK/'native-all.log').read_text(errors='replace')[-400:],flush=True)
def sha(p):
    with p.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
identity=dict(product_commit=REVISION,src_tree=subprocess.check_output(['git','rev-parse','HEAD:src'],cwd=ROOT,text=True).strip(),files={})
(WORK/'Release').mkdir(exist_ok=True)
for name in ['openttd.exe','openttd.pdb','openttd_test.exe']:
    source=BUILD/'Release'/name;dest=WORK/'Release'/name
    shutil.copy2(source,dest)
    assert sha(source)==sha(dest)
    identity['files']['Release/'+name]=dict(bytes=dest.stat().st_size,sha256=sha(dest))
for folder in ['lang','baseset','save-integrity','real-map-driver']:
    shutil.copytree(BUILD/folder,WORK/folder,dirs_exist_ok=True)
shutil.copy2(ROOT/'build-rebaseline-88cb61e3/candidate/cpu-sampled.wpaProfile',WORK/'cpu-sampled.wpaProfile')
for folder in ['symbols','symcache']:
    shutil.copytree(ROOT/'build-rebaseline-88cb61e3/candidate'/folder,WORK/folder,dirs_exist_ok=True)
identity['instrumentation']='same existing copied-TU exact-tick/WPR helpers; product routing is unchanged'
identity['compiled_from']=str(BUILD)
(WORK/'build-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
(WORK/'build-ready.txt').write_text('Requested revision built and native tests passed\n')
print('f9c4a85b WPR executable/PDB prepared',flush=True)
