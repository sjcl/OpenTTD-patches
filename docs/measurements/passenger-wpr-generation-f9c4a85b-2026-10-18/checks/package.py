"""Validate two normal WPR captures and retain inputs, ETLs and matching symbols."""
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
NAME='passenger-wpr-generation-f9c4a85b-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
DEST=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329')/('passenger-wpr-recordings-f9c4a85b-2026-10-18')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def copy(p,q):q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
def compress(p,q):
    q.parent.mkdir(parents=True,exist_ok=True)
    with p.open('rb') as source,q.open('wb') as raw:
        with gzip.GzipFile(fileobj=raw,mode='wb',filename='',mtime=0) as dest:shutil.copyfileobj(source,dest)
assert (WORK/'analysis-complete.txt').is_file()
identity=load(WORK/'build-identity.json');inputs=load(WORK/'input-validation.json');summary=load(OUT/'summary.json')
assert identity['product_commit']=='f9c4a85b721073bbf86747f4a4cec7ac5ccd2f89'
assert inputs['copied_without_conversion'] and inputs['source_sha256']==inputs['frozen_input_sha256']
native=(WORK/'native-all.log').read_text(errors='replace')
assert 'Passed all 763 test cases with 10586649 assertions.' in native
proof=dict(product_commit=identity['product_commit'],same_input_between_modes=True,input_changed_since_previous=True,
    canonical_equality_checked=False,input_not_converted=True,native_tests=763,native_assertions=10586649,runs={})
for mode in ['legacy','hyperpath']:
    local=WORK/mode;public=OUT/mode
    meta=load(local/'process.json');clock=meta['actual-ticks.json'];s=summary[mode]
    assert meta['exit_code']==0 and meta['simulation_timing_valid'] and not meta['load_failed']
    assert meta['input_sha256']==inputs['source_sha256']
    assert meta['binary_sha256']==identity['files']['Release/openttd.exe']['sha256']
    assert meta['resume_settings'] and meta['warm_complete'] and meta['background_ready']['completed']
    assert meta['background_ready']['tick_before']==meta['background_ready']['tick_after']==inputs['start_tick']
    assert not meta['cold_precomputed'] and not meta['rebuild_precomputed'] and not meta['full_rebuild_precomputed']
    assert meta['precomputed'] and meta['hyperpath']==(mode=='hyperpath') and meta['hyperpath_filter_mode']=='index'
    assert meta['main_thread_affinity_mask']==1
    assert clock['start_tick']==inputs['start_tick'] and clock['end_tick']==inputs['target_end_tick'] and len(clock['samples'])==2048
    assert meta['save-integrity.json']['payments_valid']
    audit=load(local/'window-audit.json');assert audit['valid'] and not audit['overlapping_competing_processes']
    assert s['lost_events']==s['lost_buffers']==0 and not s['unresolved_product_functions']
    warning_times=[int(t) for warning in s['wpa_invalid_event_warning'] for t in re.findall(r'TimeStamp:\s*(\d+)',warning)]
    assert len(warning_times)==len(s['wpa_invalid_event_warning'])
    assert all(t<audit['marker_begin_us'] or t>audit['marker_end_us'] for t in warning_times)
    export=load(local/'export-command.json')
    for name,entry in export['exports'].items():
        data=gzip.decompress((public/(name+'.gz')).read_bytes())
        assert len(data)==entry['bytes'] and hashlib.sha256(data).hexdigest()==entry['sha256']
    for name in ['window-audit.json','window-markers.txt']:copy(local/name,public/name)
    proof['runs'][mode]=dict(exact_ticks=2048,input_sha256=meta['input_sha256'],simulation_timing_valid=True,
        whole_process_timing_valid=meta['whole_process_timing_valid'],phase_contamination=meta['phase_contamination'],
        kernel_window_valid=True,lost_events=0,lost_buffers=0,unresolved_product_functions={},
        canonical_equality_checked=False,payments_valid=True,raw_csv_hashes_match=True,
        conservation_checks=s['conservation_checks'],unavailable_stack=s['unavailable_stack'],
        wpa_invalid_event_warning=s['wpa_invalid_event_warning'],invalid_event_times_us=warning_times,
        invalid_events_outside_markers=True)
    validation=WORK/('validation-'+mode)
    for name in ['process.json','save-integrity.json']:copy(validation/name,OUT/'checks'/('validation-'+mode)/name)
    compress(validation/'on-1-console.txt',OUT/'checks'/('validation-'+mode)/'on-1-console.txt.gz')
    compress(validation/'stderr.txt',OUT/'checks'/('validation-'+mode)/'stderr.txt.gz')
    copy(WORK/(mode+'-validation.log'),OUT/'checks'/(mode+'-validation.log'))
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/trace-stats.txt -whitespace\n**/window-markers.txt -whitespace\n')
for name in ['build-identity.json','build-command.json','runs-complete.json','input-validation.json','cpu-sampled.wpaProfile','wpr-recorder.log','wpr-recorder-complete.txt']:
    copy(WORK/name,OUT/name)
for p in WORK.glob('*.py'):copy(p,OUT/'checks'/p.name)
copy(WORK/'record.ps1',OUT/'checks/record.ps1')
for name in ['configure.log','build.log','prepare-runner.log','export-runner.log','summary-runner.log','freeze-runner.log']:
    compress(WORK/name,OUT/'checks'/(name+'.gz'))
for name in ['native-all.log','validation-runner.log']:copy(WORK/name,OUT/'checks'/name)
copy(WORK/'legacy/export-command-rejected-sandbox.json',OUT/'checks/failed-export/export-command.json')
for name in ['legacy/wpa-export-rejected-sandbox.log','export-rejected-sandbox-runner.log']:
    compress(WORK/name,OUT/'checks/failed-export'/(Path(name).name+'.gz'))
for folder in ['save-integrity','real-map-driver']:
    for p in (WORK/folder).iterdir():
        if p.is_file():copy(p,OUT/'checks'/folder/p.name)
for name in ['package.json','openttd.cfg']:copy(WORK/'input'/name,OUT/'input'/name)
DEST.mkdir(parents=True,exist_ok=True)
package=WORK/'input';manifest=load(package/'package.json')
expected={e['path']:e['sha256'] for e in manifest['assets']}
expected.update({'input.sav':inputs['source_sha256'],'openttd.cfg':inputs['config_sha256'],
    'package.json':sha(package/'package.json')})
assert all(sha(package/name)==value for name,value in expected.items())
input_zip=DEST/'input.zip'
if not input_zip.exists():
    with zipfile.ZipFile(input_zip,'w',compression=zipfile.ZIP_STORED) as z:
        for name in sorted(expected):z.write(package/name,name)
        z.writestr('RESTORE.txt','Extract this archive to recreate the fixed input package. input.sav is an unchanged byte-for-byte copy of gc3833d807.sav. Both modes use this same file directly; validation result saves are not used as inputs. Use the matching build language directory with --lang.\n')
with zipfile.ZipFile(input_zip) as z:
    for name,value in expected.items():
        with z.open(name) as stream:assert hashlib.file_digest(stream,'sha256').hexdigest()==value,name
input_zip.chmod(stat.S_IREAD)
backup=dict(path=str(input_zip),bytes=input_zip.stat().st_size,sha256=sha(input_zip),
    verified_entries=len(expected),input_sha256=inputs['source_sha256'],input_not_converted=True,read_only=True)
(OUT/'input-backup.json').write_text(json.dumps(backup,indent=2)+'\n')
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
for name in ['Release/openttd.exe','Release/openttd.pdb','cpu-sampled.wpaProfile','build-identity.json','input-validation.json']:
    copy(WORK/name,DEST/name)
    assert sha(WORK/name)==sha(DEST/name)
    entries[name]=dict(bytes=(DEST/name).stat().st_size,sha256=sha(DEST/name))
recordings=dict(path=str(DEST),files=entries,matching_binary_and_pdb_copied=True,traces_read_only=True,
    note='ETLs retained with NTFS hard links where possible; either archive link survives removal of the worktree link')
(DEST/'recordings-manifest.json').write_text(json.dumps(recordings,indent=2)+'\n')
(OUT/'recording-backup.json').write_text(json.dumps(recordings,indent=2)+'\n')
(OUT/'validation.json').write_text(json.dumps(proof,indent=2)+'\n')
print('Two ordinary traces, matching binaries, original input and raw CSV hashes verified',flush=True)
