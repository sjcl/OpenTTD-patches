from pathlib import Path
import subprocess,sys,json,os
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'build-clang-thin-pgo'
plain=len(sys.argv)>1 and sys.argv[1]=='plain'
for mode in ('baseline','pgo'):
    name=f"{'plain' if plain else 'wpr'}-{mode}-1"
    command=[sys.executable,str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',str(BASE/mode/'Release/openttd.exe'),str(BASE/'input'),str(BASE/'runs'/name),
        '--lang',str(BASE/mode/'lang'),'--ticks','1024','--timeout','900','--resume','--wait-saved-replay','--precomputed','--cold-precomputed','--warm-precomputed']
    if not plain:command+=['--wpr-profile']
    print('START',name,flush=True)
    env=dict(os.environ,LLVM_PROFILE_FILE=str(BASE/'profiles/verification-%p-%m.profraw'))
    (BASE/(name+'-command.json')).write_text(json.dumps(dict(command=command,environment={'LLVM_PROFILE_FILE':env['LLVM_PROFILE_FILE']}),indent=2)+'\n')
    with (BASE/(name+'.log')).open('wb') as log:subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    record=json.loads((BASE/'runs'/name/'process.json').read_text())
    assert record['exit_code']==0 and record['simulation_timing_valid'] and record['warm_complete'] and record['save-integrity.json']['payments_valid']
    print('DONE',name,flush=True)
