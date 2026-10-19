"""Verify actual Clang compile command logs, not just requested CMake options."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
BUILD=ROOT/'build-wpr-clang-thin-2026-10-19/compile'
proof={}
for target in ['openttd_lib','openttd','openttd_test']:
    paths=list((BUILD/(target+'.dir')/'Release').rglob('clang-cl.command.1.tlog'))
    assert paths,target
    commands=[]
    for path in paths:
        data=path.read_bytes()
        text=data.decode('utf-16') if data.startswith((b'\xff\xfe',b'\xfe\xff')) else data.decode('utf-8-sig')
        commands.extend(line for line in text.splitlines() if line and not line.startswith('^'))
    assert commands,target
    for command in commands:
        for flag in ['O2','Ob2','MT','Z7']:
            assert re.search(r'(?:^|[\s"])[/-]'+flag+r'(?:[\s"]|$)',command),(target,flag,command[:300])
        assert '-flto=thin' in command and 'NDEBUG' in command and '-fprofile' not in command and 'UNDEBUG' not in command,target
    proof[target]=dict(command_count=len(commands),all_required_flags_present=True,example=commands[0],
        logs={p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in paths})
project=json.loads((WORK/'project-flags.json').read_text(encoding='utf-8'))
link_xml=project['openttd']['release_xml'].replace('\\','/')
candidates=[p for p in Path('C:/Program Files/LLVM/lib/clang/23/lib').rglob('clang_rt.builtins*.lib') if p.as_posix() in link_xml]
assert len(candidates)==1,candidates
runtime=candidates[0]
with runtime.open('rb') as stream:runtime_sha=hashlib.file_digest(stream,'sha256').hexdigest()
proof['builtins']=dict(path=str(runtime),sha256=runtime_sha,bytes=runtime.stat().st_size)
(WORK/'actual-compile-flags.json').write_text(json.dumps(proof,indent=2)+'\n',encoding='utf-8',newline='\n')
print('Actual compile flags and builtins verified',flush=True)
