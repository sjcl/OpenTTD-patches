"""Validate and retain the two profiles, matching binaries and reproducible evidence."""
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
OLD=ROOT/'build-rebaseline-88cb61e3'
NAME='passenger-wpr-generation-8ff4834d-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
DEST=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329/passenger-wpr-recordings-8ff4834d-2026-10-18')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def copy(p,q):q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
def compress(p,q):
    q.parent.mkdir(parents=True,exist_ok=True)
    with p.open('rb') as source,q.open('wb') as raw:
        with gzip.GzipFile(fileobj=raw,mode='wb',filename='',mtime=0) as dest:shutil.copyfileobj(source,dest)
assert (WORK/'analysis-complete.txt').is_file()
identity=load(WORK/'build-identity.json');seeds=load(WORK/'input-conversion-validation.json');summary=load(OUT/'summary.json')
proof=dict(product_commit=identity['product_commit'],canonical_equality_checked=False,runs={})
for mode in ['legacy','hyperpath']:
    local=WORK/mode;public=OUT/mode
    meta=load(local/'process.json');clock=meta['actual-ticks.json'];s=summary[mode]
    assert meta['exit_code']==0 and meta['simulation_timing_valid'] and not meta['load_failed']
    assert meta['input_sha256']==seeds[mode]['input_sha256']
    assert meta['binary_sha256']==identity['files']['Release/openttd.exe']['sha256']
    assert meta['resume_settings'] and meta['warm_complete'] and meta['background_ready']['completed']
    assert meta['background_ready']['tick_before']==meta['background_ready']['tick_after']
    assert not meta['cold_precomputed'] and not meta['rebuild_precomputed'] and not meta['full_rebuild_precomputed']
    assert meta['precomputed'] and meta['hyperpath']==(mode=='hyperpath') and meta['hyperpath_filter_mode']=='index'
    assert meta['main_thread_affinity_mask']==1
    assert clock['start_tick']==13647872 and clock['end_tick']==13649920 and len(clock['samples'])==2048
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
(OUT/'checks').mkdir(parents=True,exist_ok=True)
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/trace-stats.txt -whitespace\n**/window-markers.txt -whitespace\n')
for name in ['build-identity.json','build-command.json','runs-complete.json','input-conversion.json','input-conversion-validation.json','cpu-sampled.wpaProfile','wpr-recorder.log','wpr-recorder-complete.txt']:
    copy(WORK/name,OUT/name)
for p in WORK.glob('*.py'):copy(p,OUT/'checks'/p.name)
copy(WORK/'record.ps1',OUT/'checks/record.ps1')
for name in ['configure.log','build.log','prepare-runner.log','export-runner.log','summary-runner.log']:
    compress(WORK/name,OUT/'checks'/(name+'.gz'))
copy(WORK/'native-all.log',OUT/'checks/native-all.log')
for folder in ['save-integrity','real-map-driver']:
    for p in (WORK/folder).iterdir():
        if p.is_file():copy(p,OUT/'checks'/folder/p.name)
for mode in ['legacy','hyperpath']:
    copy(WORK/'inputs'/mode/'package.json',OUT/'inputs'/mode/'package.json')
    copy(WORK/'inputs'/mode/'openttd.cfg',OUT/'inputs'/mode/'openttd.cfg')
    validation_dir=WORK/('conversion-validation-'+mode)
    for name in ['process.json','save-integrity.json']:
        copy(validation_dir/name,OUT/'checks'/('conversion-validation-'+mode)/name)
    compress(validation_dir/'on-1-console.txt',OUT/'checks'/('conversion-validation-'+mode)/'on-1-console.txt.gz')
    copy(WORK/(mode+'-conversion-validation.log'),OUT/'checks'/(mode+'-conversion-validation.log'))
for name in ['conversion-validation-runner.log','rejected-input-measure-runner.log']:
    copy(WORK/name,OUT/'checks'/name)
for name in ['process.json']:
    p=WORK/'rejected-old-development-save'/name
    if p.is_file():copy(p,OUT/'checks'/'rejected-old-development-save'/name)
for name in ['stderr.txt','on-1-console.txt']:
    p=WORK/'rejected-old-development-save'/name
    if p.is_file():compress(p,OUT/'checks'/'rejected-old-development-save'/(name+'.gz'))
backup=load(OLD/'input-backup.json');assert sha(Path(backup['path']))==backup['sha256']
copy(OLD/'input-backup.json',OUT/'input-backup.json')
proof['input_backup_sha_verified']=True
if not DEST.exists():DEST.mkdir(parents=True)
input_zip=DEST/'converted-inputs.zip'
if not input_zip.exists():
    with zipfile.ZipFile(input_zip,'w',compression=zipfile.ZIP_STORED) as z:
        for mode in ['legacy','hyperpath']:
            for name in ['input.sav','package.json','openttd.cfg']:
                z.write(WORK/'inputs'/mode/name,mode+'/'+name)
        for name in ['input-conversion.json','input-conversion-validation.json','convert_inputs.py']:
            z.write(WORK/name,name)
        z.writestr('RESTORE.txt','Restore shared assets from the original backup recorded in input-backup.json. Overlay each mode with input.sav, openttd.cfg and package.json from this ZIP. No simulation ticks were advanced by the version conversion. The source saves in the original backup are unchanged.\n')
        z.writestr('input-backup.json',json.dumps(backup,indent=2)+'\n')
with zipfile.ZipFile(input_zip) as z:
    for mode in ['legacy','hyperpath']:
        assert hashlib.sha256(z.read(mode+'/input.sav')).hexdigest()==seeds[mode]['input_sha256']
converted_backup=dict(path=str(input_zip),bytes=input_zip.stat().st_size,sha256=sha(input_zip),
    input_hashes={mode:seeds[mode]['input_sha256'] for mode in seeds},shared_assets_backup=backup,
    verified_converted_saves=True)
(OUT/'converted-input-backup.json').write_text(json.dumps(converted_backup,indent=2)+'\n')
entries={}
for mode in ['legacy','hyperpath']:
    source=WORK/mode/'cpu.etl';dest=DEST/mode/'cpu.etl';dest.parent.mkdir(parents=True,exist_ok=True)
    expected=load(WORK/mode/'export-command.json')['trace_sha256']
    if not dest.exists():
        try:os.link(source,dest)
        except OSError:shutil.copy2(source,dest)
    assert sha(dest)==expected
    dest.chmod(stat.S_IREAD)
    entries[dest.relative_to(DEST).as_posix()]=dict(bytes=dest.stat().st_size,sha256=expected,read_only=True)
    for name in ['process.json','save-integrity.json','actual-ticks.json','window-audit.json']:
        copy(WORK/mode/name,DEST/mode/name)
for name in ['Release/openttd.exe','Release/openttd.pdb','cpu-sampled.wpaProfile','build-identity.json']:
    copy(WORK/name,DEST/name)
    assert sha(WORK/name)==sha(DEST/name)
    entries[name]=dict(bytes=(DEST/name).stat().st_size,sha256=sha(DEST/name))
recordings=dict(path=str(DEST),files=entries,matching_binary_and_pdb_copied=True,traces_read_only=True,
    note='ETLs retained with NTFS hard links where possible; either archive link survives removal of the worktree link')
(DEST/'recordings-manifest.json').write_text(json.dumps(recordings,indent=2)+'\n')
(OUT/'recording-backup.json').write_text(json.dumps(recordings,indent=2)+'\n')
(OUT/'validation.json').write_text(json.dumps(proof,indent=2)+'\n')
print('Both ordinary WPR captures, raw CSVs and durable recordings verified',flush=True)
