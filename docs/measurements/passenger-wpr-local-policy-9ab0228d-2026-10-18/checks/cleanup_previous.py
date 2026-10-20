"""Delete user-requested previous, untracked measurement data after both recordings finish."""
from pathlib import Path
import json
import os
import shutil
import stat
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
ARCHIVES=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329').resolve()
previous=['build-rebaseline-88cb61e3','build-wpr-88cb61e3','build-wpr-8ff4834d','build-wpr-f9c4a85b','build-merge-jgrpp-ya-20261018']
archive_names=['passenger-wpr-recordings-88cb61e3-2026-10-18','passenger-wpr-recordings-8ff4834d-2026-10-18',
    'passenger-wpr-recordings-f9c4a85b-2026-10-18','passenger-wpr-inputs-88cb61e3-2026-10-18.zip']
deadline=time.monotonic()+10800
while True:
    try:
        done=set(json.loads((WORK/'runs-complete.json').read_text()))=={'legacy','hyperpath'} and (WORK/'wpr-recorder-complete.txt').is_file()
    except (FileNotFoundError,json.JSONDecodeError):done=False
    if done:break
    if time.monotonic()>deadline:raise TimeoutError('Recordings did not finish; no previous results deleted')
    time.sleep(2)
assert (WORK/'tools/wpt/Windows Kits/10/Windows Performance Toolkit/wpaexporter.exe').is_file()
tracked=subprocess.check_output(['git','ls-files','-z'],cwd=ROOT,text=True).split('\0')
for name in previous:
    assert not any(p==name or p.startswith(name+'/') for p in tracked),name
    assert subprocess.run(['git','check-ignore','--quiet',name+'/'],cwd=ROOT).returncode==0,name
targets=[]
for base,names in [(ROOT,previous),(ARCHIVES,archive_names)]:
    for name in names:
        path=(base/name).resolve()
        assert path.parent==base and path!=WORK and not path.is_relative_to(WORK)
        assert path.is_relative_to(base) and not path.is_symlink() and not path.is_junction()
        targets.append(path)
plan=dict(request='User requested deletion of all previous untracked measurement results',
    keep=[str(WORK),'Git-tracked reports, CSVs, canonical results and source files','Original gc3833d807.sav'],
    free_before=shutil.disk_usage(ROOT).free,targets=[])
for path in targets:
    files=[p for p in path.rglob('*') if p.is_file()] if path.is_dir() else ([path] if path.is_file() else [])
    plan['targets'].append(dict(path=str(path),files=len(files),logical_bytes=sum(p.stat().st_size for p in files),
        exists=path.exists(),validated_within_named_root=True,removed=False))
(WORK/'previous-results-cleanup.json').write_text(json.dumps(plan,indent=2)+'\n')
def make_writable(function,path,error):
    p=Path(path).absolute()
    assert any(p==t or p.is_relative_to(t) for t in targets),p
    os.chmod(path,stat.S_IWRITE|stat.S_IREAD)
    function(path)
for path,item in zip(targets,plan['targets']):
    if path.is_dir():shutil.rmtree(path,onexc=make_writable)
    elif path.is_file():
        path.chmod(stat.S_IWRITE|stat.S_IREAD);path.unlink()
    assert not path.exists(),path
    item['removed']=True
    (WORK/'previous-results-cleanup.json').write_text(json.dumps(plan,indent=2)+'\n')
plan['free_after']=shutil.disk_usage(ROOT).free
plan['free_change_bytes']=plan['free_after']-plan['free_before']
plan['all_previous_targets_removed']=True
(WORK/'previous-results-cleanup.json').write_text(json.dumps(plan,indent=2)+'\n')
(WORK/'cleanup-complete.txt').write_text('All nine named previous untracked result/build/archive targets removed\n')
print(json.dumps(plan,indent=2),flush=True)
