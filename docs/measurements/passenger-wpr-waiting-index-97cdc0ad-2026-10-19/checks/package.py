"""Validate exact-window evidence and retain only this run's necessary raw artifacts."""
from pathlib import Path
import gzip
import hashlib
import json
import os
import re
import shutil
import stat

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18'
DEST=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329/passenger-wpr-recordings-97cdc0ad-2026-10-19')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def copy(p,q):q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
def compress(p,q):
    q.parent.mkdir(parents=True,exist_ok=True)
    with p.open('rb') as source,q.open('wb') as raw:
        with gzip.GzipFile(fileobj=raw,mode='wb',filename='',mtime=0) as dest:shutil.copyfileobj(source,dest)
assert (WORK/'analysis-complete.txt').is_file() and (WORK/'wpr-recorder-complete.txt').is_file()
identity=load(WORK/'build-identity.json');inputs=load(WORK/'input-identity.json')
summary=load(OUT/'summary.json');canonical=load(OUT/'canonical-comparison.json')
assert identity['product_commit']=='97cdc0ad4c9493daea0db6079e90a03924d96b56'
assert inputs['same_as_9ab0228d'] and inputs['copied_without_conversion']
assert sha(WORK/'input/package.json')==sha(BASE/'input/package.json')
native=(WORK/'native-all.log').read_text(errors='replace')
tests=re.search(r'Passed all (\d+) test cases with (\d+) assertions\.',native);assert tests
proof=dict(product_commit=identity['product_commit'],baseline_commit='9ab0228db17178ef931de961305ebec544162f6a',
    same_input=True,canonical_equality_checked=True,canonical=canonical,native_tests=int(tests[1]),native_assertions=int(tests[2]),runs={})
for mode in ['legacy','hyperpath']:
    local=WORK/mode;public=OUT/mode;meta=load(local/'process.json');clock=meta['actual-ticks.json'];s=summary[mode]
    assert meta['exit_code']==0 and meta['simulation_timing_valid'] and not meta['load_failed']
    assert meta['input_sha256']==inputs['input_sha256'] and meta['binary_sha256']==identity['files']['Release/openttd.exe']['sha256']
    assert meta['resume_settings'] and meta['warm_complete'] and meta['background_ready']['completed']
    assert meta['background_ready']['tick_before']==meta['background_ready']['tick_after']==inputs['start_tick']
    assert not meta['cold_precomputed'] and not meta['rebuild_precomputed'] and not meta['full_rebuild_precomputed']
    assert meta['precomputed'] and meta['hyperpath']==(mode=='hyperpath') and meta['hyperpath_filter_mode']=='index'
    assert meta['main_thread_affinity_mask']==1
    assert clock['start_tick']==inputs['start_tick'] and clock['end_tick']==inputs['target_end_tick'] and len(clock['samples'])==2048
    assert meta['save-integrity.json']['payments_valid']
    old=load(BASE/mode/'save-integrity.json');new=meta['save-integrity.json']
    assert len(old)==len(new)==24 and (old==new)==canonical[mode]['equal']
    audit=load(local/'window-audit.json');assert audit['valid'] and not audit['overlapping_competing_processes']
    assert s['lost_events']==s['lost_buffers']==0 and not s['unresolved_product_functions']
    warning_times=[]
    for warning in s['wpa_invalid_event_warning']:
        for block in re.split(r'(?=Warning: EventSink)',warning):
            if not block.strip():continue
            times=re.findall(r'TimeStamp:\s*(\d+)',block)
            assert block.startswith('Warning: EventSink') and len(times)==1
            warning_times.append(int(times[0]))
    assert all(t<audit['marker_begin_us'] or t>audit['marker_end_us'] for t in warning_times)
    export=load(local/'export-command.json')
    for name,entry in export['exports'].items():
        data=gzip.decompress((public/(name+'.gz')).read_bytes())
        assert len(data)==entry['bytes'] and hashlib.sha256(data).hexdigest()==entry['sha256']
    for name in ['window-audit.json','window-markers.txt']:copy(local/name,public/name)
    proof['runs'][mode]=dict(exact_ticks=2048,kernel_window_valid=True,canonical_equal=canonical[mode]['equal'],canonical_fields=24,
        payments_valid=True,raw_csv_hashes_match=True,lost_events=0,lost_buffers=0,unresolved_product_functions={},
        conservation_checks=s['conservation_checks'],unavailable_stack=s['unavailable_stack'],
        invalid_event_times_us=warning_times,invalid_events_outside_markers=True)
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/trace-stats.txt -whitespace\n**/window-markers.txt -whitespace\n')
for name in ['build-identity.json','build-command.json','runs-complete.json','input-identity.json','recorder-settings.json',
    'cpu-sampled.wpaProfile','cpu-memory.wprp','cpu-file.wprp','profile-interval.txt','wpr-recorder.log','wpr-recorder-complete.txt']:
    copy(WORK/name,OUT/name)
for p in WORK.glob('*.py'):copy(p,OUT/'checks'/p.name)
copy(WORK/'record.ps1',OUT/'checks/record.ps1')
for name in ['configure.log','build.log','prepare-runner.log','measure-runner.log','export-runner.log','summary-runner.log','freeze-runner.log']:
    compress(WORK/name,OUT/'checks'/(name+'.gz'))
copy(WORK/'native-all.log',OUT/'checks/native-all.log')
for folder in ['save-integrity','real-map-driver']:
    for p in (WORK/folder).iterdir():
        if p.is_file():compress(p,OUT/'checks'/folder/(p.name+'.gz'))
for name in ['openttd_lib.vcxproj','openttd.vcxproj','CMakeCache.txt']:
    compress(WORK/'compile'/name,OUT/'checks'/(name+'.gz'))
for name in ['package.json','openttd.cfg']:copy(WORK/'input'/name,OUT/'input'/name)
backup=load(BASE/'input-backup.json');archive=Path(backup['path'])
assert archive.is_file() and sha(archive)==backup['sha256'] and backup['input_sha256']==inputs['input_sha256']
backup['note']='Identical frozen input archive retained from the 9ab0228d capture; no duplicate ZIP created'
(OUT/'input-backup.json').write_text(json.dumps(backup,indent=2)+'\n')
DEST.mkdir(parents=True,exist_ok=True);entries={}
for mode in ['legacy','hyperpath']:
    source=WORK/mode/'cpu.etl';dest=DEST/mode/'cpu.etl';dest.parent.mkdir(parents=True,exist_ok=True)
    expected=load(WORK/mode/'export-command.json')['trace_sha256']
    if not dest.exists():
        try:os.link(source,dest)
        except OSError:shutil.copy2(source,dest)
    assert sha(dest)==expected;dest.chmod(stat.S_IREAD)
    entries[dest.relative_to(DEST).as_posix()]=dict(bytes=dest.stat().st_size,sha256=expected,read_only=True)
    for name in ['process.json','save-integrity.json','actual-ticks.json','window-audit.json']:copy(WORK/mode/name,DEST/mode/name)
for name in ['Release/openttd.exe','Release/openttd.pdb','cpu-sampled.wpaProfile','build-identity.json','input-identity.json','recorder-settings.json','cpu-memory.wprp','cpu-file.wprp']:
    copy(WORK/name,DEST/name);assert sha(WORK/name)==sha(DEST/name)
    entries[name]=dict(bytes=(DEST/name).stat().st_size,sha256=sha(DEST/name))
recordings=dict(path=str(DEST),files=entries,matching_binary_and_pdb_copied=True,traces_read_only=True)
(DEST/'recordings-manifest.json').write_text(json.dumps(recordings,indent=2)+'\n')
(OUT/'recording-backup.json').write_text(json.dumps(recordings,indent=2)+'\n')
(OUT/'validation.json').write_text(json.dumps(proof,indent=2)+'\n')
print('Both captures, canonical fields, CPU windows and raw export hashes verified',flush=True)
