from pathlib import Path
import subprocess,json
BASE=Path(__file__).resolve().parent
cases=[('default-off',['-DCHECK_COMPILER=MSVC'],0,''),
    ('invalid-mode',['-DCHECK_COMPILER=Clang','-DOPTION_PGO=INVALID'],1,'must be OFF, GENERATE, or USE'),
    ('unsupported-compiler',['-DCHECK_COMPILER=MSVC','-DOPTION_PGO=GENERATE'],1,'requires Clang'),
    ('missing-profile',['-DCHECK_COMPILER=Clang','-DOPTION_PGO=USE'],1,'requires an existing PGO_PROFILE_FILE'),
    ('profile-is-directory',['-DCHECK_COMPILER=Clang','-DOPTION_PGO=USE','-DPGO_PROFILE_FILE='+BASE.as_posix()],1,'requires an existing PGO_PROFILE_FILE')]
results=[]
for name,args,status,expected in cases:
    command=['cmake']+args+['-P',str(BASE/'check-options.cmake')]
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    assert (result.returncode!=0)==bool(status) and expected in result.stdout,(name,result.stdout)
    results.append(dict(name=name,command=command,return_code=result.returncode,output=result.stdout))
(BASE/'option-checks.json').write_text(json.dumps(results,indent=2)+'\n')
print('PASS: PGO default, invalid mode, unsupported compiler, missing profile, directory profile')
