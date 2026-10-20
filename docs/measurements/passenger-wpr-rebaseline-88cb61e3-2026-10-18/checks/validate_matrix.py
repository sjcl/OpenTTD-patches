"""Check accepted traces, exact ticks, canonical equivalence and export conservation."""
from pathlib import Path
import gzip
import hashlib
import json
import re
import shutil

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OUT=ROOT/'docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18'
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
checks=dict(seed_reload_fields=24,runs={},exports={},canonical={})
seeds=load(WORK/'seeds-complete.json')['seeds']
for mode in ['legacy','hyperpath']:
    package=WORK/'seeds'/mode;manifest=load(package/'package.json')
    assert sha(package/'input.sav')==seeds[mode]['input_sha256']==manifest['input_sha256']
    assert len(seeds[mode]['canonical'])==24 and seeds[mode]['reload_equal']
    assert seeds[mode]['frozen_tick']==13647872
    a=load(OUT/'baseline'/mode/'save-integrity.json');b=load(OUT/'candidate'/mode/'save-integrity.json')
    assert a==b and len(a)==len(b)==24
    checks['canonical'][mode]=dict(equal=True,fields=24)
    for version in ['baseline','candidate']:
        local=WORK/version/mode;public=OUT/version/mode
        meta=load(local/'process.json');clock=meta['actual-ticks.json'];summary=load(OUT/version/'summary.json')[mode]
        assert meta['exit_code']==0 and meta['simulation_timing_valid'] is True and not meta['load_failed']
        assert meta['warm_complete'] is True and meta['resume_settings'] is True and meta['cold_precomputed'] is False
        assert meta['hyperpath']==(mode=='hyperpath') and meta['hyperpath_filter_mode']=='index'
        assert meta['main_thread_affinity_mask']==1 and meta['input_sha256']==seeds[mode]['input_sha256']
        assert clock['start_tick']==13647872 and clock['end_tick']==13649920 and len(clock['samples'])==2048
        assert meta['background_ready']['completed'] and meta['background_ready']['tick_before']==meta['background_ready']['tick_after']
        audit=load(local/'window-audit.json');assert audit['valid'] and not audit['overlapping_competing_processes']
        assert summary['lost_events']==summary['lost_buffers']==0 and not summary['unresolved_product_functions']
        warning_times=[int(t) for warning in summary['wpa_invalid_event_warning'] for t in re.findall(r'TimeStamp:\s*(\d+)',warning)]
        assert len(warning_times)==len(summary['wpa_invalid_event_warning'])
        assert all(t<audit['marker_begin_us'] or t>audit['marker_end_us'] for t in warning_times)
        shutil.copy2(local/'window-audit.json',public/'window-audit.json')
        shutil.copy2(local/'window-markers.txt',public/'window-markers.txt')
        export=load(local/'export-command.json')
        for name,entry in export['exports'].items():
            zipped=public/(name+'.gz')
            data=gzip.decompress(zipped.read_bytes())
            assert len(data)==entry['bytes'] and hashlib.sha256(data).hexdigest()==entry['sha256']
        checks['runs'][version+'/'+mode]=dict(exact_ticks=2048,start=13647872,end=13649920,
            simulation_timing_valid=meta['simulation_timing_valid'],whole_process_timing_valid=meta['whole_process_timing_valid'],
            phase_contamination=meta['phase_contamination'],kernel_window_valid=True,
            lost_events=0,lost_buffers=0,unresolved_product_functions={})
        checks['exports'][version+'/'+mode]=dict(raw_tables=len(export['exports']),gzip_hashes_match=True,
            conservation_checks=summary['conservation_checks'],unavailable_stack=summary['unavailable_stack'],
            wpa_invalid_event_warning=summary['wpa_invalid_event_warning'])
        checks['exports'][version+'/'+mode]['invalid_event_times_us']=warning_times
        checks['exports'][version+'/'+mode]['invalid_events_outside_markers']=True
backup=load(WORK/'input-backup.json');assert sha(Path(backup['path']))==backup['sha256']
checks['input_backup_verified']=True
(OUT/'validation.json').write_text(json.dumps(checks,indent=2)+'\n')
for name in ['canonical-comparison.json','idle-orphan-cleanup.json']:
    shutil.copy2(WORK/name,OUT/name)
for version in ['baseline','candidate']:
    for name in ['wpr-recorder.log','wpr-recorder-complete.txt']:
        shutil.copy2(WORK/version/name,OUT/'checks'/(version+'-'+name))
    with (WORK/version/'export-runner.log').open('rb') as src,(OUT/'checks'/(version+'-export-runner.log.gz')).open('wb') as raw:
        with gzip.GzipFile(fileobj=raw,mode='wb',filename='',mtime=0) as dest:shutil.copyfileobj(src,dest)
shutil.copy2(WORK/'matrix-recorder-complete.txt',OUT/'checks/matrix-recorder-complete.txt')
print('All four WPR captures validated; canonical 24/24 per mode; all exported tables conserved')
