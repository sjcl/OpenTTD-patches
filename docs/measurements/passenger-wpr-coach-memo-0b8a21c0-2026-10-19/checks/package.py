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
NAME='passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
DEST=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329/passenger-wpr-recordings-0b8a21c0-2026-10-19')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def copy(p,q):q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
def compress(p,q):
    q.parent.mkdir(parents=True,exist_ok=True)
    with p.open('rb') as source,q.open('wb') as raw:
        with gzip.GzipFile(fileobj=raw,mode='wb',filename='',mtime=0) as dest:shutil.copyfileobj(source,dest)
assert (WORK/'analysis-complete.txt').is_file() and (WORK/'wpr-recorder-complete.txt').is_file()
assert (WORK/'validate-complete.txt').is_file() and (WORK/'hyperpath-buffered-complete.txt').is_file()
assert (WORK/'wpr-buffered-recorder-complete.txt').is_file()
identity=load(WORK/'build-identity.json');inputs=load(WORK/'input-identity.json')
summary=load(OUT/'summary.json');canonical=load(OUT/'canonical-comparison.json')
assert identity['product_commit']=='0b8a21c0f3dc9a24ca0d9d3581c19fb9c207e684'
assert inputs['same_as_97cdc0ad'] and inputs['copied_without_conversion']
assert sha(WORK/'input/package.json')==sha(BASE/'input/package.json')
native=(WORK/'native-all.log').read_text(errors='replace')
tests=re.search(r'Passed all (\d+) test cases with (\d+) assertions\.',native);assert tests
proof=dict(product_commit=identity['product_commit'],baseline_commit='97cdc0ad4c9493daea0db6079e90a03924d96b56',
    same_input=True,canonical_equality_checked=True,canonical=canonical,native_tests=int(tests[1]),native_assertions=int(tests[2]),runs={})
for mode in ['legacy','hyperpath']:
    local=WORK/mode;public=OUT/mode;meta=load(local/'process.json');clock=meta['actual-ticks.json'];s=summary[mode]
    assert meta['exit_code']==0 and meta['simulation_timing_valid'] and not meta['load_failed']
    assert meta['input_sha256']==inputs['input_sha256'] and meta['binary_sha256']==identity['files']['Release/openttd.exe']['sha256']
    assert meta['resume_settings'] and meta['warm_complete'] and meta['background_ready']['completed']
    assert meta['saved_replay']['ready']
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
    'cpu-sampled.wpaProfile','cpu-memory.wprp','cpu-file.wprp','cpu-file-buffered.wprp','buffer-adjustment.json','profile-interval.txt','prepare-compat.json',
    'wpr-recorder.log','wpr-recorder-complete.txt','wpr-legacy-retry-recorder.log','wpr-legacy-retry-recorder-complete.txt',
    'wpr-buffered-recorder.log','wpr-buffered-recorder-complete.txt','validate-result.json']:
    copy(WORK/name,OUT/name)
for p in WORK.glob('*.py'):copy(p,OUT/'checks'/p.name)
copy(WORK/'record.ps1',OUT/'checks/record.ps1')
copy(WORK/'record_legacy_retry.ps1',OUT/'checks/record_legacy_retry.ps1')
copy(WORK/'record_legacy_file.ps1',OUT/'checks/record_legacy_file.ps1')
copy(WORK/'record_buffered.ps1',OUT/'checks/record_buffered.ps1')
for name in ['configure.log','build.log','prepare-runner.log','prepare-anchor-failure.log','legacy-recorder-start-failure.log','measure-runner.log','export-runner.log','summary-runner.log','freeze-runner.log','validate-runner.log','validate-controller.log']:
    compress(WORK/name,OUT/'checks'/(name+'.gz'))
copy(WORK/'native-all.log',OUT/'checks/native-all.log')
for folder in ['save-integrity','real-map-driver']:
    for p in (WORK/folder).iterdir():
        if p.is_file():compress(p,OUT/'checks'/folder/(p.name+'.gz'))
for name in ['openttd_lib.vcxproj','openttd.vcxproj','CMakeCache.txt']:
    compress(ROOT/'build-wpr-97cdc0ad/compile'/name,OUT/'checks'/(name+'.gz'))
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
validate=load(WORK/'validate-result.json');vm=load(WORK/'validate/process.json')
assert validate['exact_ticks']==2048 and validate['input_sha256']==inputs['input_sha256']
assert vm['binary_sha256']==identity['files']['Release/openttd.exe']['sha256']
assert vm['background_ready']['completed'] and vm['background_ready']['tick_before']==vm['background_ready']['tick_after']==inputs['start_tick']
assert vm['saved_replay']['ready']
assert validate['excluded_from_performance_comparison'] and validate['hyperpath_filter_mode']=='validate'
for name in ['process.json','save-integrity.json','actual-ticks.json']:copy(WORK/'validate'/name,OUT/'validate'/name)
for name in ['on-1-console.txt','stderr.txt','stdout.txt']:compress(WORK/'validate'/name,OUT/'validate'/(name+'.gz'))
proof['validate']=validate
(OUT/'validation.json').write_text(json.dumps(proof,indent=2)+'\n')
rejected=(WORK/'rejected-incomplete-save-replay').resolve()
assert rejected.parent==WORK and rejected.name=='rejected-incomplete-save-replay'
if rejected.exists():
    assert not rejected.is_symlink() and not rejected.is_junction()
    aborted=load(rejected/'aborted.json');assert aborted['performance_accepted'] is False
    for name in ['aborted.json','process.json','save-integrity.json','actual-ticks.json','wpr-ready.txt','wpr-go.txt','wpr-finished.txt','wpr-saved.txt']:
        if (rejected/name).is_file():copy(rejected/name,OUT/'checks/rejected-incomplete-save-replay'/name)
    for name in ['on-1-console.txt','stderr.txt','stdout.txt']:
        if (rejected/name).is_file():compress(rejected/name,OUT/'checks/rejected-incomplete-save-replay'/(name+'.gz'))
    for name in ['rejected-measure-runner.log','rejected-legacy-runner.log']:
        compress(WORK/name,OUT/'checks/rejected-incomplete-save-replay'/(name+'.gz'))
    aborted.update(raw_etl_bytes=(rejected/'cpu.etl').stat().st_size,raw_artifacts_removed=True,thin_evidence_preserved=True)
    (OUT/'rejected-attempt.json').write_text(json.dumps(aborted,indent=2)+'\n')
    def remove_readonly(function,path,error):
        assert Path(path).absolute().is_relative_to(rejected)
        os.chmod(path,stat.S_IWRITE|stat.S_IREAD);function(path)
    shutil.rmtree(rejected,onexc=remove_readonly)
    assert not rejected.exists()
else:
    assert load(OUT/'rejected-attempt.json')['raw_artifacts_removed']
memory_rejected=(WORK/'rejected-legacy-memory-markers').resolve()
assert memory_rejected.parent==WORK and memory_rejected.name=='rejected-legacy-memory-markers'
if memory_rejected.exists():
    assert not memory_rejected.is_symlink() and not memory_rejected.is_junction()
    marks=(memory_rejected/'window-markers.txt').read_text(encoding='utf-8-sig')
    assert 'legacy-end' in marks and 'legacy-begin' not in marks
    assert load(memory_rejected/'save-integrity.json')==load(BASE/'legacy/save-integrity.json')
    for name in ['process.json','save-integrity.json','actual-ticks.json','window-markers.txt','wpr-ready.txt','wpr-go.txt','wpr-finished.txt','wpr-saved.txt']:
        copy(memory_rejected/name,OUT/'checks/rejected-legacy-memory-markers'/name)
    for name in ['on-1-console.txt','stderr.txt','stdout.txt']:
        compress(memory_rejected/name,OUT/'checks/rejected-legacy-memory-markers'/(name+'.gz'))
    for name in ['rejected-legacy-memory-runner.log','rejected-legacy-window-runner.log']:
        compress(WORK/name,OUT/'checks/rejected-legacy-memory-markers'/(name+'.gz'))
    rejected_info=dict(performance_accepted=False,reason='ETL legacy-begin marker missing',canonical_equal=True,
        raw_etl_bytes=(memory_rejected/'cpu.etl').stat().st_size,raw_artifacts_removed=True,thin_evidence_preserved=True)
    (OUT/'rejected-memory-attempt.json').write_text(json.dumps(rejected_info,indent=2)+'\n')
    def remove_memory_readonly(function,path,error):
        assert Path(path).absolute().is_relative_to(memory_rejected)
        os.chmod(path,stat.S_IWRITE|stat.S_IREAD);function(path)
    shutil.rmtree(memory_rejected,onexc=remove_memory_readonly)
    assert not memory_rejected.exists()
else:
    assert load(OUT/'rejected-memory-attempt.json')['raw_artifacts_removed']
lost_rejected=(WORK/'rejected-hyperpath-dropped-events').resolve()
assert lost_rejected.parent==WORK and lost_rejected.name=='rejected-hyperpath-dropped-events'
if lost_rejected.exists():
    assert not lost_rejected.is_symlink() and not lost_rejected.is_junction()
    assert load(lost_rejected/'save-integrity.json')==load(BASE/'hyperpath/save-integrity.json')
    for name in ['process.json','save-integrity.json','actual-ticks.json','wpr-ready.txt','wpr-go.txt','wpr-finished.txt','wpr-saved.txt']:
        copy(lost_rejected/name,OUT/'checks/rejected-hyperpath-dropped-events'/name)
    for name in ['on-1-console.txt','stderr.txt','stdout.txt']:
        compress(lost_rejected/name,OUT/'checks/rejected-hyperpath-dropped-events'/(name+'.gz'))
    compress(WORK/'rejected-hyperpath-runner.log',OUT/'checks/rejected-hyperpath-dropped-events/runner.log.gz')
    info=dict(performance_accepted=False,reason='WPR reported 2024287 dropped events',canonical_equal=True,
        raw_etl_bytes=(lost_rejected/'cpu.etl').stat().st_size,raw_artifacts_removed=True,thin_evidence_preserved=True)
    (OUT/'rejected-dropped-events.json').write_text(json.dumps(info,indent=2)+'\n')
    def remove_lost_readonly(function,path,error):
        assert Path(path).absolute().is_relative_to(lost_rejected)
        os.chmod(path,stat.S_IWRITE|stat.S_IREAD);function(path)
    shutil.rmtree(lost_rejected,onexc=remove_lost_readonly)
    assert not lost_rejected.exists()
else:
    assert load(OUT/'rejected-dropped-events.json')['raw_artifacts_removed']
print('Both captures, canonical fields, CPU windows and raw export hashes verified',flush=True)
