from pathlib import Path
import subprocess,re,json,hashlib
BASE=Path(__file__).resolve().parent
LLVM=Path(r'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin')
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
summary=(BASE/'profiles/summary.txt').read_text(encoding='utf-8-sig')
assert 'Instrumentation level: IR' in summary,summary
for name in ('StateGameLoop','PlanPrecomputed','CanBoard'):
    with (BASE/'profiles'/f'{name}.txt').open('wb') as log:
        subprocess.run([str(LLVM/'llvm-profdata.exe'),'show','--counts','--function='+name,str(BASE/'profiles/real-map.profdata')],stdout=log,stderr=subprocess.STDOUT,check=True)
    selected=(BASE/'profiles'/f'{name}.txt').read_text(encoding='utf-8-sig')
    assert name in selected
    assert any(int(n)>0 for group in re.findall(r'Block counts: \[([^\]]+)\]',selected) for n in re.findall(r'\d+',group)),name
result={}
for mode in ('baseline','pgo'):
    result[mode]={}
    for name in ('openttd','passenger_precomputed','passenger_service_database'):
        obj=BASE/mode/'openttd_lib.dir/Release'/(name+'.obj')
        out=BASE/mode/(name+'.ll')
        command=[str(LLVM/'clang.exe'),'-S','-emit-llvm','-x','ir',str(obj),'-o',str(out)]
        subprocess.run(command,check=True)
        text=out.read_text(encoding='utf-8')
        entries=len(re.findall(r'!"function_entry_count"',text))
        branches=len(re.findall(r'!"branch_weights"',text))
        has_summary='!"ProfileSummary"' in text
        assert has_summary==(mode=='pgo') and ((entries>0)==(mode=='pgo')),(mode,name,entries,has_summary)
        world_entry=None
        if mode=='pgo' and name=='openttd':
            definition=next(line for line in text.splitlines() if 'define ' in line and 'StateGameLoop@@' in line)
            prof=re.search(r'!prof !(\d+)',definition)
            assert prof,definition
            world_entry=int(re.search(r'^!'+prof[1]+r' = !\{!"function_entry_count", i64 (\d+)',text,re.M)[1])
            assert world_entry>=4096,world_entry
        selected=[line for line in text.splitlines() if '!"ProfileSummary"' in line or '!"function_entry_count"' in line or ('define ' in line and 'StateGameLoop@@' in line)]
        result[mode][name]=dict(command=command,object_sha256=sha(obj),llvm_ir_sha256=sha(out),profile_summary=has_summary,function_entry_count_records=entries,branch_weight_records=branches,world_entry_count=world_entry,excerpt=selected[:30])
(BASE/'profile-use-checks.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS: IR training profile; selected runtime functions covered; profile summaries and entry counts exist only in PGO objects')
