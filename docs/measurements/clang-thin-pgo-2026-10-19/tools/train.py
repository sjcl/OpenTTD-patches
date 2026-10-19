from pathlib import Path
import subprocess,os,sys,json,hashlib,time
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'build-clang-thin-pgo'
profiles=BASE/'profiles'
raw=profiles/'raw';raw.mkdir(parents=True,exist_ok=True)
assert not list(raw.glob('*.profraw')),'Refusing to mix an older training profile'
env=dict(os.environ,LLVM_PROFILE_FILE=str(raw/'real-map-%p-%m.profraw'))
command=[sys.executable,str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',str(BASE/'generate/Release/openttd.exe'),str(BASE/'input'),str(BASE/'training'),
    '--lang',str(BASE/'generate/lang'),'--ticks','4096','--timeout','1800','--resume','--wait-saved-replay','--precomputed','--cold-precomputed','--warm-precomputed']
print('TRAIN START 4096 ticks',flush=True)
started=time.time()
with (BASE/'training.log').open('wb') as log:subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
record=json.loads((BASE/'training/process.json').read_text())
assert record['exit_code']==0 and record['warm_complete'] and record['simulation_timing_valid'] and record['save-integrity.json']['payments_valid']
assert record['actual-ticks.json']['end_tick']-record['actual-ticks.json']['start_tick']==4096
files=sorted(raw.glob('*.profraw'))
assert files and all(p.stat().st_size>0 and p.stat().st_mtime>=started for p in files)
tool=r'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/llvm-profdata.exe'
merge=[tool,'merge','-o',str(profiles/'real-map.profdata')]+[str(p) for p in files]
subprocess.run(merge,check=True)
with (profiles/'summary.txt').open('wb') as log:subprocess.run([tool,'show','--detailed-summary','--topn=20',str(profiles/'real-map.profdata')],stdout=log,stderr=subprocess.STDOUT,check=True)
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
identity=dict(command=command,environment={'LLVM_PROFILE_FILE':env['LLVM_PROFILE_FILE']},merge_command=merge,
    binary_sha256=sha(BASE/'generate/Release/openttd.exe'),input_sha256=sha(BASE/'input/input.sav'),
    raw_profiles={str(p):dict(sha256=sha(p),size=p.stat().st_size) for p in files},profile_sha256=sha(profiles/'real-map.profdata'))
(profiles/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
print('TRAIN COMPLETE',identity['profile_sha256'],flush=True)
