"""Validate current captures, keep their artifacts, and record the requested previous-data cleanup."""
from pathlib import Path
import gzip
import hashlib
import json
import os
import re
import shutil
import stat
import zipfile

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-local-policy-9ab0228d-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-generation-f9c4a85b-2026-10-18'
DEST=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329/passenger-wpr-recordings-9ab0228d-2026-10-18')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def copy(p,q):q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
def compress(p,q):
    q.parent.mkdir(parents=True,exist_ok=True)
    with p.open('rb') as source,q.open('wb') as raw:
        with gzip.GzipFile(fileobj=raw,mode='wb',filename='',mtime=0) as dest:shutil.copyfileobj(source,dest)
assert (WORK/'analysis-complete.txt').is_file() and (WORK/'cleanup-complete.txt').is_file()
assert (WORK/'hyperpath-retry-complete.txt').is_file() and (WORK/'wpr-retry-recorder-complete.txt').is_file()
identity=load(WORK/'build-identity.json');inputs=load(WORK/'input-identity.json');summary=load(OUT/'summary.json')
cleanup=load(WORK/'previous-results-cleanup.json');canonical=load(OUT/'canonical-comparison.json')
assert identity['product_commit']=='9ab0228db17178ef931de961305ebec544162f6a'
assert inputs['same_as_f9c4a85b'] and inputs['copied_without_conversion']
assert cleanup['all_previous_targets_removed'] and all(x['removed'] and not Path(x['path']).exists() for x in cleanup['targets'])
native=(WORK/'native-all.log').read_text(errors='replace')
tests=re.search(r'Passed all (\d+) test cases with (\d+) assertions\.',native);assert tests
proof=dict(product_commit=identity['product_commit'],same_input=True,canonical_equality_checked=True,
    canonical=canonical,native_tests=int(tests[1]),native_assertions=int(tests[2]),previous_results_deleted=True,runs={})
assert sha(WORK/'input/package.json')==sha(BASE/'input/package.json')
for mode in ['legacy','hyperpath']:
    local=WORK/mode;public=OUT/mode
    meta=load(local/'process.json');clock=meta['actual-ticks.json'];s=summary[mode]
    assert meta['exit_code']==0 and meta['simulation_timing_valid'] and not meta['load_failed']
    assert meta['input_sha256']==inputs['input_sha256']
    assert meta['binary_sha256']==identity['files']['Release/openttd.exe']['sha256']
    assert meta['resume_settings'] and meta['warm_complete'] and meta['background_ready']['completed']
    assert meta['background_ready']['tick_before']==meta['background_ready']['tick_after']==inputs['start_tick']
    assert not meta['cold_precomputed'] and not meta['rebuild_precomputed'] and not meta['full_rebuild_precomputed']
    assert meta['precomputed'] and meta['hyperpath']==(mode=='hyperpath') and meta['hyperpath_filter_mode']=='index'
    assert meta['main_thread_affinity_mask']==1
    assert clock['start_tick']==inputs['start_tick'] and clock['end_tick']==inputs['target_end_tick'] and len(clock['samples'])==2048
    assert meta['save-integrity.json']['payments_valid']
    old=load(BASE/mode/'save-integrity.json');new=meta['save-integrity.json']
    assert len(old)==len(new)==24
    assert (old==new)==canonical[mode]['equal']
    audit=load(local/'window-audit.json');assert audit['valid'] and not audit['overlapping_competing_processes']
    assert s['lost_events']==s['lost_buffers']==0 and not s['unresolved_product_functions']
    warning_blocks=[block for warning in s['wpa_invalid_event_warning']
        for block in re.split(r'(?=Warning: EventSink)',warning) if block.strip()]
    warning_times=[]
    for block in warning_blocks:
        times=re.findall(r'TimeStamp:\s*(\d+)',block)
        assert block.startswith('Warning: EventSink') and len(times)==1
        warning_times.append(int(times[0]))
    assert all(t<audit['marker_begin_us'] or t>audit['marker_end_us'] for t in warning_times)
    export=load(local/'export-command.json')
    for name,entry in export['exports'].items():
        data=gzip.decompress((public/(name+'.gz')).read_bytes())
        assert len(data)==entry['bytes'] and hashlib.sha256(data).hexdigest()==entry['sha256']
    for name in ['window-audit.json','window-markers.txt']:copy(local/name,public/name)
    proof['runs'][mode]=dict(exact_ticks=2048,input_sha256=meta['input_sha256'],simulation_timing_valid=True,
        whole_process_timing_valid=meta['whole_process_timing_valid'],phase_contamination=meta['phase_contamination'],
        kernel_window_valid=True,lost_events=0,lost_buffers=0,unresolved_product_functions={},
        canonical_equal=canonical[mode]['equal'],canonical_fields=24,payments_valid=True,raw_csv_hashes_match=True,
        conservation_checks=s['conservation_checks'],unavailable_stack=s['unavailable_stack'],
        wpa_invalid_event_warning=s['wpa_invalid_event_warning'],invalid_event_times_us=warning_times,
        invalid_events_outside_markers=True)
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/trace-stats.txt -whitespace\n**/window-markers.txt -whitespace\n')
for name in ['build-identity.json','build-command.json','runs-complete.json','input-identity.json','cpu-sampled.wpaProfile',
    'wpr-recorder.log','wpr-recorder-complete.txt','wpr-retry-recorder.log','wpr-retry-recorder-complete.txt',
    'previous-results-cleanup.json','cache-cleanup.json','tools-preserved.json','rejected-marker.json',
    'wpr-memory-retry-recorder.log','wpr-memory-retry-recorder-complete.txt','buffer-change.json','recorder-settings.json','marker-byte-diagnostic.json',
    'wpr-buffered-memory-retry-recorder.log','wpr-buffered-memory-retry-recorder-complete.txt']:
    copy(WORK/name,OUT/name)
for p in WORK.glob('*.py'):copy(p,OUT/'checks'/p.name)
copy(WORK/'record.ps1',OUT/'checks/record.ps1')
copy(WORK/'record_retry.ps1',OUT/'checks/record_retry.ps1')
copy(WORK/'record_memory_retry.ps1',OUT/'checks/record_memory_retry.ps1')
copy(WORK/'record_buffered_memory_retry.ps1',OUT/'checks/record_buffered_memory_retry.ps1')
for name in ['cpu-memory.wprp','cpu-memory-large-events.wprp','cpu-file.wprp','wpr-start-help.txt']:
    copy(WORK/name,OUT/'checks'/name)
rejections=[]
for folder,log in [('rejected-missing-begin-marker','export-rejected-marker-runner.log'),
    ('rejected-missing-begin-marker-2','export-rejected-marker-2-runner.log'),
    ('rejected-missing-begin-marker-3','export-rejected-marker-3-runner.log')]:
    rejected=(WORK/folder).resolve()
    assert rejected.parent==WORK and rejected.name==folder
    assert not rejected.is_symlink() and not rejected.is_junction()
    for name in ['window-markers.txt','trace-stats.txt','process.json','save-integrity.json','wpr-ready.txt','wpr-go.txt','wpr-finished.txt','wpr-saved.txt']:
        copy(rejected/name,OUT/'checks'/folder/name)
    for name in ['on-1-console.txt','stderr.txt']:
        compress(rejected/name,OUT/'checks'/folder/(name+'.gz'))
    compress(WORK/log,OUT/'checks'/folder/'export-runner.log.gz')
    rejections.append(dict(performance_accepted=False,reason='hyperpath-begin missing from ETL marks',
        original_etl_bytes=(rejected/'cpu.etl').stat().st_size,canonical_equal=True,
        retake_validated=True,thin_evidence_preserved=True,discarded_run_deleted=True))
    def remove_rejected_readonly(function,path,error):
        assert Path(path).absolute().is_relative_to(rejected)
        os.chmod(path,stat.S_IWRITE|stat.S_IREAD);function(path)
    shutil.rmtree(rejected,onexc=remove_rejected_readonly)
    assert not rejected.exists()
(OUT/'rejected-attempts.json').write_text(json.dumps(rejections,indent=2)+'\n')
for name in ['configure.log','build.log','prepare-runner.log','resume-prepare-runner.log','export-runner.log','summary-runner.log','freeze-runner.log','cleanup-runner.log']:
    compress(WORK/name,OUT/'checks'/(name+'.gz'))
copy(WORK/'native-all.log',OUT/'checks/native-all.log')
for folder in ['save-integrity','real-map-driver']:
    for p in (WORK/folder).iterdir():
        if p.is_file():copy(p,OUT/'checks'/folder/p.name)
for name in ['package.json','openttd.cfg']:copy(WORK/'input'/name,OUT/'input'/name)
DEST.mkdir(parents=True,exist_ok=True)
package=WORK/'input';manifest=load(package/'package.json')
expected={e['path']:e['sha256'] for e in manifest['assets']}
expected.update({'input.sav':inputs['input_sha256'],'openttd.cfg':inputs['config_sha256'],'package.json':sha(package/'package.json')})
assert all(sha(package/name)==value for name,value in expected.items())
archive=DEST/'input.zip'
if not archive.exists():
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_STORED) as z:
        for name in sorted(expected):z.write(package/name,name)
        z.writestr('RESTORE.txt','Extract to restore the current 9ab0228d fixed input package. input.sav is unchanged gc3833d807.sav and identical to the f9c4a85b input. Use the matching build language directory with --lang. Previous measurement backups were deleted at the user request; only this current capture is retained.\n')
with zipfile.ZipFile(archive) as z:
    for name,value in expected.items():
        with z.open(name) as stream:assert hashlib.file_digest(stream,'sha256').hexdigest()==value,name
archive.chmod(stat.S_IREAD)
(OUT/'input-backup.json').write_text(json.dumps(dict(path=str(archive),bytes=archive.stat().st_size,sha256=sha(archive),
    input_sha256=inputs['input_sha256'],verified_entries=len(expected),read_only=True),indent=2)+'\n')
entries={}
for mode in ['legacy','hyperpath']:
    source=WORK/mode/'cpu.etl';dest=DEST/mode/'cpu.etl';dest.parent.mkdir(parents=True,exist_ok=True)
    expected_hash=load(WORK/mode/'export-command.json')['trace_sha256']
    if not dest.exists():
        try:os.link(source,dest)
        except OSError:shutil.copy2(source,dest)
    assert sha(dest)==expected_hash
    dest.chmod(stat.S_IREAD)
    entries[dest.relative_to(DEST).as_posix()]=dict(bytes=dest.stat().st_size,sha256=expected_hash,read_only=True)
    for name in ['process.json','save-integrity.json','actual-ticks.json','window-audit.json']:copy(WORK/mode/name,DEST/mode/name)
for name in ['Release/openttd.exe','Release/openttd.pdb','cpu-sampled.wpaProfile','build-identity.json','input-identity.json']:
    copy(WORK/name,DEST/name)
    assert sha(WORK/name)==sha(DEST/name)
    entries[name]=dict(bytes=(DEST/name).stat().st_size,sha256=sha(DEST/name))
recordings=dict(path=str(DEST),files=entries,matching_binary_and_pdb_copied=True,traces_read_only=True,
    note='Only the current 9ab0228d artifacts are retained. Old untracked measurements and external backups were deleted at the user request.')
(DEST/'recordings-manifest.json').write_text(json.dumps(recordings,indent=2)+'\n')
(OUT/'recording-backup.json').write_text(json.dumps(recordings,indent=2)+'\n')
(OUT/'validation.json').write_text(json.dumps(proof,indent=2)+'\n')
print('Two current captures and canonical comparisons verified; previous untracked results removed',flush=True)
