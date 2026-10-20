"""Validate and preserve the four matched kernel-CPU traces and independent memo validation."""
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
HIST=ROOT/'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
DEST=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329/passenger-wpr-recordings-0b8a21c0-2026-10-19')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def copy(p,q):q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
def compress(p,q):
    q.parent.mkdir(parents=True,exist_ok=True)
    with p.open('rb') as source,q.open('wb') as raw:
        with gzip.GzipFile(fileobj=raw,mode='wb',filename='',mtime=0) as dest:shutil.copyfileobj(source,dest)
assert all((WORK/n).is_file() for n in ['analysis-complete.txt','validate-complete.txt','final-captures-complete.txt'])
identity=load(WORK/'build-identity.json');inputs=load(WORK/'input-identity.json');jobs=load(WORK/'final-controls.json')
assert identity['product_commit']=='0b8a21c0f3dc9a24ca0d9d3581c19fb9c207e684' and len(jobs)==4
assert {r['profile_sha256'] for r in jobs.values()}=={sha(WORK/'cpu-minimal.wprp')}
assert inputs['same_as_97cdc0ad'] and inputs['copied_without_conversion'] and inputs['assets_verified']==245
assert sha(WORK/'input/package.json')==sha(HIST/'input/package.json')
native=(WORK/'native-all.log').read_text(errors='replace');tests=re.search(r'Passed all (\d+) test cases with (\d+) assertions\.',native);assert tests
proof=dict(product_commit=identity['product_commit'],baseline_commit='97cdc0ad4c9493daea0db6079e90a03924d96b56',
    same_input=True,canonical_equality_checked=True,canonical=load(OUT/'canonical-comparison.json'),
    native_tests=int(tests[1]),native_assertions=int(tests[2]),matched_kernel_cpu_profile=True,accepted_traces=4,runs={},control_runs={})
entries={}
for role,local_root,public in [('target',WORK,OUT),('control',WORK/'control-97cdc0ad',OUT/'control-97cdc0ad')]:
    summary=load(public/'summary.json');build=load(local_root/'build-identity.json')
    if role=='control':assert build==load(HIST/'build-identity.json')
    for mode in ['legacy','hyperpath']:
        local=local_root/mode;meta=load(local/'process.json');ticks=meta['actual-ticks.json'];s=summary[mode]
        assert meta['exit_code']==0 and meta['simulation_timing_valid'] and meta['warm_complete'] and meta['background_ready']['completed']
        assert meta['input_sha256']==inputs['input_sha256'] and meta['binary_sha256']==build['files']['Release/openttd.exe']['sha256']
        assert meta['background_ready']['tick_before']==meta['background_ready']['tick_after']==inputs['start_tick']
        assert ticks['start_tick']==inputs['start_tick'] and ticks['end_tick']==inputs['target_end_tick'] and len(ticks['samples'])==2048
        assert meta['resume_settings'] and meta['precomputed'] and meta['hyperpath']==(mode=='hyperpath') and meta['hyperpath_filter_mode']=='index' and meta['main_thread_affinity_mask']==1
        assert not meta['cold_precomputed'] and not meta['rebuild_precomputed'] and not meta['full_rebuild_precomputed']
        if role=='target':assert meta['saved_replay']['ready']
        old=load(HIST/mode/'save-integrity.json');actual=meta['save-integrity.json'];assert len(old)==len(actual)==24 and actual['payments_valid']
        differences={k:[old.get(k),actual.get(k)] for k in sorted(old.keys()|actual.keys()) if old.get(k)!=actual.get(k)}
        audit=load(local/'window-audit.json');assert audit['valid'] and not audit['overlapping_competing_processes']
        assert s['lost_events']==s['lost_buffers']==0 and not s['unresolved_product_functions']
        warning_times=[int(t) for warning in s['wpa_invalid_event_warning'] for t in re.findall(r'TimeStamp:\s*(\d+)',warning)]
        assert all(t<audit['marker_begin_us'] or t>audit['marker_end_us'] for t in warning_times)
        export=load(local/'export-command.json')
        for name,item in export['exports'].items():
            data=gzip.decompress((public/mode/(name+'.gz')).read_bytes())
            assert len(data)==item['bytes'] and hashlib.sha256(data).hexdigest()==item['sha256']
        for name in ['window-audit.json','window-markers.txt','recorder-trace-stats.txt','recorder-markers.txt']:copy(local/name,public/mode/name)
        result=dict(exact_ticks=2048,kernel_window_valid=True,canonical_equal=not differences,canonical_fields=24,differences=differences,
            payments_valid=True,raw_csv_hashes_match=True,lost_events=0,lost_buffers=0,unresolved_product_functions={},
            conservation_checks=s['conservation_checks'],unavailable_stack=s['unavailable_stack'],invalid_event_times_us=warning_times,invalid_events_outside_markers=True)
        proof['runs' if role=='target' else 'control_runs'][mode]=result
        dest=DEST/role/mode/'cpu.etl';dest.parent.mkdir(parents=True,exist_ok=True)
        if not dest.exists():
            try:os.link(local/'cpu.etl',dest)
            except OSError:shutil.copy2(local/'cpu.etl',dest)
        assert sha(dest)==export['trace_sha256'];dest.chmod(stat.S_IREAD)
        entries[dest.relative_to(DEST).as_posix()]=dict(bytes=dest.stat().st_size,sha256=export['trace_sha256'],read_only=True)
        for name in ['process.json','save-integrity.json','actual-ticks.json','window-audit.json']:copy(local/name,DEST/role/mode/name)
    for name in ['Release/openttd.exe','Release/openttd.pdb','build-identity.json']:
        copy(local_root/name,DEST/role/name)
        entries[(Path(role)/name).as_posix()]=dict(bytes=(DEST/role/name).stat().st_size,sha256=sha(DEST/role/name))
validate=load(WORK/'validate-result.json');vm=load(WORK/'validate/process.json')
assert validate['exact_ticks']==2048 and validate['input_sha256']==inputs['input_sha256'] and validate['hyperpath_filter_mode']=='validate'
assert vm['binary_sha256']==identity['files']['Release/openttd.exe']['sha256'] and vm['saved_replay']['ready']
assert vm['background_ready']['tick_before']==vm['background_ready']['tick_after']==inputs['start_tick']
assert validate['excluded_from_performance_comparison'];proof['validate']=validate
proof['memo_validation_passed']=validate['reuse_observed'] and validate['memo']['violations']==0
(OUT/'validation.json').write_text(json.dumps(proof,indent=2)+'\n')
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/trace-stats.txt -whitespace\n**/window-markers.txt -whitespace\n**/recorder-*.txt -whitespace\n')
for name in ['build-identity.json','build-command.json','input-identity.json','cpu-sampled.wpaProfile','cpu-minimal.wprp','minimal-profile.json','profile-interval.txt',
    'prepare-compat.json','failed-raw-cleanup.json','final-controls.json','validate-result.json','wpr-minimal-recorder.log']:
    copy(WORK/name,OUT/name)
copy(WORK/'control-97cdc0ad/build-identity.json',OUT/'control-97cdc0ad/build-identity.json')
for p in WORK.glob('*.py'):copy(p,OUT/'checks'/p.name)
for p in WORK.glob('record*.ps1'):copy(p,OUT/'checks'/p.name)
for name in ['configure.log','build.log','prepare-runner.log','prepare-anchor-failure.log','minimal-matched-runner.log','export-runner.log','summary-runner.log','control-summary-runner.log','finish-analysis-runner.log','validate-runner.log','validate-controller.log']:
    compress(WORK/name,OUT/'checks'/(name+'.gz'))
copy(WORK/'native-all.log',OUT/'checks/native-all.log')
for folder in ['save-integrity','real-map-driver']:
    for p in (WORK/folder).iterdir():
        if p.is_file():compress(p,OUT/'checks'/folder/(p.name+'.gz'))
for folder in sorted(WORK.glob('rejected-*')):
    if not folder.is_dir():continue
    assert folder.resolve().parent==WORK and not folder.is_symlink() and not folder.is_junction()
    for name in ['process.json','save-integrity.json','actual-ticks.json','aborted.json','window-markers.txt','wpr-ready.txt','wpr-go.txt','wpr-finished.txt','wpr-saved.txt']:
        if (folder/name).is_file():copy(folder/name,OUT/'checks'/folder.name/name)
    for name in ['on-1-console.txt','stderr.txt','stdout.txt']:
        if (folder/name).is_file():compress(folder/name,OUT/'checks'/folder.name/(name+'.gz'))
for p in WORK.glob('wpr-*.log'):
    if p.name!='wpr-minimal-recorder.log':compress(p,OUT/'checks'/(p.name+'.gz'))
for p in (WORK/'sampling-jobs').glob('*/quality.json'):copy(p,OUT/'checks/sampling-jobs'/p.parent.name/'quality.json')
for name in ['process.json','save-integrity.json','actual-ticks.json']:copy(WORK/'validate'/name,OUT/'validate'/name)
for name in ['on-1-console.txt','stderr.txt','stdout.txt']:compress(WORK/'validate'/name,OUT/'validate'/(name+'.gz'))
for name in ['package.json','openttd.cfg']:copy(WORK/'input'/name,OUT/'input'/name)
backup=load(HIST/'input-backup.json');assert sha(Path(backup['path']))==backup['sha256']
(OUT/'input-backup.json').write_text(json.dumps(backup,indent=2)+'\n')
for name in ['cpu-minimal.wprp','minimal-profile.json','cpu-sampled.wpaProfile']:copy(WORK/name,DEST/name)
(OUT/'recording-backup.json').write_text(json.dumps(dict(path=str(DEST),files=entries,matched_profile='cpu-minimal.wprp',accepted_traces=4),indent=2)+'\n')
print('Four matched zero-loss traces and independent validate run verified',flush=True)
