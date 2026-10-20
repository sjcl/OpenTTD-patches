"""Keep accepted traces, two builds and two validate runs independently of the worktree."""
from pathlib import Path
import gzip,hashlib,json,re,shutil,stat
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-abba-88dc128a-2026-10-20'
OUT=ROOT/'docs/measurements'/NAME
INPUT=ROOT/'build-wpr-clang-thin-2026-10-19/input'
BACKUP=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329')/'passenger-wpr-recordings-abba-88dc128a-2026-10-20'
ROUNDS=['01-A1','02-B1','03-B2','04-A2']
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def dump(p,x):p.write_text(json.dumps(x,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def copy(a,b):b.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(a,b)
assert (WORK/'analysis-complete.txt').is_file()
captures=load(WORK/'capture-results.json');validation={};counter=0
for name in ROUNDS:
 summary=load(OUT/name/'summary.json')
 for mode in ['legacy','hyperpath']:
  counter+=1;key=name+'/'+mode;run=WORK/name/mode;dest=OUT/name/mode;s=summary[mode]
  assert s['lost_events']==s['lost_buffers']==0 and not s['unresolved_product_functions'] and s['ticks']==2048 and s['payments_valid']
  audit=load(run/'window-audit.json');assert audit['valid']
  warnings=[int(t) for warning in s['wpa_invalid_event_warning'] for t in re.findall(r'TimeStamp:\s*(\d+)',warning)]
  assert all(t<audit['marker_begin_us'] or t>audit['marker_end_us'] for t in warnings)
  exported=load(dest/'export-command.json');assert exported['exit_code']==0
  for fn,item in exported['exports'].items():
   with gzip.open(dest/(fn+'.gz'),'rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==item['sha256']
  for fn in ['window-audit.json','window-markers.txt','window-processes.txt','recorder-markers.txt','recorder-trace-stats.txt']:copy(run/fn,dest/fn)
  copy(WORK/'sampling-jobs'/f'{counter:02}-{name}-{mode}'/'quality.json',dest/'recorder-quality.json')
  assert all(x['equal'] for x in captures[key]['canonical'].values())
  validation[key]=dict(exact_ticks=2048,canonical=captures[key]['canonical'],window_valid=True,lost_events=0,lost_buffers=0,raw_csv_hashes_match=True,unresolved_product_functions={},invalid_event_times_us=warnings,invalid_events_outside_markers=True,conservation_checks=s['conservation_checks'],payments_valid=True)
native={};validates={}
for label in ['A','B']:
 saved=WORK/label;v=load(saved/'validate-result.json');assert v['passed'] and v['counters_passed'];validates[label]=v
 for fn in ['process.json','actual-ticks.json','save-integrity.json','on-1-console.txt']:copy(saved/'validate'/fn,OUT/label/'validate'/fn)
 for fn in ['validate-result.json','validate-runner.log','build-command.json','build-identity.json','prepare-compat.json','native-all.log','configure.log','build.log','project-flags.json','lto-objects.json','manifest-verification.json','manifest.log','openttd.exe.manifest.xml','openttd_test.exe.manifest.xml','pdb-scope-check.txt','pdb-preflight.json','actual-compile-flags.json']:copy(saved/fn,OUT/label/fn)
 for p in (saved/'command-logs').glob('*.gz'):copy(p,OUT/label/'command-logs'/p.name)
 copy(saved/'estimated_prepare_compat.py',OUT/'checks'/('estimated_prepare_compat_'+label+'.py'))
 tests=re.findall(r'Passed all (\d+) test cases with (\d+) assertions\.',(saved/'native-all.log').read_text(encoding='utf-8',errors='replace'));assert len(tests)==1
 native[label]=dict(tests=int(tests[0][0]),assertions=int(tests[0][1]))
dump(OUT/'validation.json',dict(accepted_traces=8,native_tests=native,same_input=True,same_capture_profile=True,runs=validation,real_map_validate=validates,canonical_rule='Same-version same-mode repetitions only; cross-version differences expected'))
for fn in ['prepare_A.py','prepare_B.py','record.ps1','measure.py','post.py','analyze.py','package.py','report.py','verify.py','probe_A.py','probe_B.py','check_build_A.py','check_build_B.py','validate.py']:copy(WORK/fn,OUT/'checks'/fn)
for fn in ['thinlto-override.json','input-identity.json','capture-results.json','plan.json','capture-tool-identity.json','profile-interval.txt','recorder.log','cpu-minimal.wprp','cpu-sampled.wpaProfile','export-runner.log']:copy(WORK/fn,OUT/fn)
for fn in ['package.json','openttd.cfg']:copy(INPUT/fn,OUT/'input'/fn)
copy(ROOT/'docs/measurements/passenger-wpr-abba-55f1cfe0-2026-10-20/input-backup.json',OUT/'input-backup.json')
for fn in ['quality.json','job.json']:copy(WORK/'sampling-jobs/00-pilot'/fn,OUT/'checks/pilot'/fn)
for fn in ['recorder-markers.txt','recorder-trace-stats.txt']:copy(WORK/'pilot'/fn,OUT/'checks/pilot'/fn)
for fn in ['prepare-runner.log','prepare-runner-retry.log']:
 if (WORK/'A'/fn).is_file():copy(WORK/'A'/fn,OUT/'checks/initial-configure'/fn)
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/*markers.txt -whitespace\n**/*trace-stats.txt -whitespace\n',encoding='utf-8',newline='\n')
BACKUP.mkdir(exist_ok=True)
backup=dict(path=str(BACKUP),files={},accepted_traces=8,validate_runs=2,profile_sha256=sha(WORK/'cpu-minimal.wprp'))
names=[label+'/'+fn for label in ['A','B'] for fn in ['Release/openttd.exe','Release/openttd.pdb','build-identity.json','validate/result.sav']]+[name+'/'+mode+'/'+fn for name in ROUNDS for mode in ['legacy','hyperpath'] for fn in ['cpu.etl','result.sav']]
for fn in names:
 p=WORK/fn;dest=BACKUP/fn
 if not dest.exists():copy(p,dest)
 assert sha(dest)==sha(p);dest.chmod(stat.S_IREAD)
 backup['files'][fn]=dict(bytes=dest.stat().st_size,sha256=sha(dest),read_only=True)
dump(OUT/'recording-backup.json',backup)
(WORK/'package-complete.txt').write_text('Independent recordings and small evidence preserved\n',encoding='utf-8',newline='\n')
print('Packaged eight traces/two validates/two build identities',flush=True)
