"""Audit all eight traces, back up raw evidence, and write the ABBA report."""
from pathlib import Path
import gzip
import hashlib
import json
import re
import shutil
import stat
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-abba-6c11c67f-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-clang-thin-merged-2026-10-19'
OLD=ROOT/'build-wpr-clang-thin-2026-10-19'
BACKUP=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329')/'passenger-wpr-recordings-abba-6c11c67f-2026-10-19'
ROUNDS=['01-A1','02-B1','03-B2','04-A2']

def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def dump(p,x):p.write_text(json.dumps(x,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def copy(a,b):b.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(a,b)
assert (WORK/'analysis-complete.txt').is_file()
captures=load(WORK/'capture-results.json');focus=load(OUT/'focus.json');comparison=load(OUT/'comparison.json');validation={}
counter=0
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
        for fn in ['window-audit.json','window-markers.txt','window-processes.txt','recorder-markers.txt','recorder-trace-stats.txt']:
            copy(run/fn,dest/fn)
        jobs=WORK/('sampling-jobs' if counter<=4 else 'sampling-jobs-resume')
        copy(jobs/f'{counter:02}-{name}-{mode}'/'quality.json',dest/'recorder-quality.json')
        assert all(x['equal'] for x in captures[key]['canonical'].values())
        validation[key]=dict(exact_ticks=2048,canonical=captures[key]['canonical'],window_valid=True,lost_events=0,lost_buffers=0,
            raw_csv_hashes_match=True,unresolved_product_functions={},invalid_event_times_us=warnings,invalid_events_outside_markers=True,
            conservation_checks=s['conservation_checks'],payments_valid=True)
tests=re.findall(r'Passed all (\d+) test cases with (\d+) assertions\.',(WORK/'native-all.log').read_text(errors='replace'));assert len(tests)==1
dump(OUT/'validation.json',dict(A_commit=comparison['A_commit'],B_commit=comparison['B_commit'],accepted_traces=8,native_tests_B=int(tests[0][0]),
    native_assertions_B=int(tests[0][1]),same_input=True,same_capture_profile=True,runs=validation))
for fn in ['prepare.py','estimated_prepare_compat.py','record.ps1','record_resume.ps1','resume.py','measure.py','post.py','analyze.py','package.py','verify.py','probe.py','check_build.py']:
    if (WORK/fn).is_file():copy(WORK/fn,OUT/'checks'/fn)
for fn in ['build-command.json','build-identity.json','baseline-identity.json','input-identity.json','prepare-compat.json','capture-results.json',
    'capture-tool-identity.json','profile-interval.txt','profile-interval-resume.txt','recorder.log','recorder-resume.log','native-all.log','configure.log','build.log','cpu-minimal.wprp','cpu-sampled.wpaProfile',
    'export-runner.log','project-flags.json','lto-objects.json','manifest-verification.json','manifest.log','openttd.exe.manifest.xml',
    'openttd_test.exe.manifest.xml','pdb-scope-check.txt','pdb-preflight.json','actual-compile-flags.json']:
    copy(WORK/fn,OUT/fn)
for target in ['openttd_lib','openttd','openttd_test']:
    p=next((OLD/'compile'/(target+'.dir')/'Release').rglob('clang-cl.command.1.tlog'))
    (OUT/'checks'/(target+'-clang-command.tlog.gz')).write_bytes(gzip.compress(p.read_bytes(),mtime=0))
for fn in ['package.json','openttd.cfg']:copy(OLD/'input'/fn,OUT/'input'/fn)
copy(BASE/'input-backup.json',OUT/'input-backup.json')
for fn in ['quality.json','job.json']:copy(WORK/'sampling-jobs/00-pilot'/fn,OUT/'checks/pilot'/fn)
for fn in ['recorder-markers.txt','recorder-trace-stats.txt']:copy(WORK/'pilot'/fn,OUT/'checks/pilot'/fn)
for fn in ['quality.json','job.json']:copy(WORK/'sampling-jobs-resume/00-pilot'/fn,OUT/'checks/pilot-resume'/fn)
for fn in ['recorder-markers.txt','recorder-trace-stats.txt']:copy(WORK/'pilot-resume'/fn,OUT/'checks/pilot-resume'/fn)
rejected=WORK/'rejected/03-B2-legacy-competition'
for fn in ['process.json','actual-ticks.json','save-integrity.json','window-audit.json','window-markers.txt','window-processes.txt','recorder-markers.txt','recorder-trace-stats.txt']:
    copy(rejected/fn,OUT/'checks/rejected-03-B2-legacy'/fn)
for fn in ['03-B2-legacy-runner.log','measure-runner.log','resume-runner.log']:
    copy(WORK/fn,OUT/'checks'/fn)
rejected_meta=load(rejected/'process.json')
rejected_canonical=load(rejected/'save-integrity.json')==load(BASE/'legacy/save-integrity.json')
assert not rejected_meta['simulation_timing_valid'] and not load(rejected/'window-audit.json')['valid']
dump(OUT/'rejection.json',dict(run='03-B2/legacy first attempt',reason='Foreign 6d2b ninja/lld-link build overlapped the marker interval; rejected before performance comparison.',
    retained_local_raw_trace=str(rejected/'cpu.etl'),raw_trace_sha256=sha(rejected/'cpu.etl'),raw_trace_bytes=(rejected/'cpu.etl').stat().st_size,
    canonical_equal_to_A=rejected_canonical,simulation_timing_valid=False,accepted_traces_not_repeated=4,
    restart='New recorder gate namespace and second pilot; retry only the contaminated run, then finish the remaining three.',
    coordination='The user had authorized coordination messages earlier in this session; Clang + LLVM版を性能比較 acknowledged holding new tests/builds until captures complete.'))
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/*markers.txt -whitespace\n**/*trace-stats.txt -whitespace\n',encoding='utf-8',newline='\n')
# A is already independently preserved by the previous task; verify its saved identity again.
baseline_backup=load(BASE/'recording-backup.json');baseline_files={}
for fn in ['Release/openttd.exe','Release/openttd.pdb']:
    p=Path(baseline_backup['path'])/fn;expected=baseline_backup['files'][fn]
    assert sha(p)==expected['sha256'];baseline_files[fn]=expected
BACKUP.mkdir(exist_ok=True)
backup=dict(path=str(BACKUP),files={},accepted_traces=8,profile_sha256=sha(WORK/'cpu-minimal.wprp'),
    A_existing_independent_backup=dict(path=baseline_backup['path'],files=baseline_files))
names=['Release/openttd.exe','Release/openttd.pdb','build-identity.json']+[name+'/'+mode+'/'+fn for name in ROUNDS for mode in ['legacy','hyperpath'] for fn in ['cpu.etl','result.sav']]
for fn in names:
    p=WORK/fn;dest=BACKUP/fn
    if not dest.exists():copy(p,dest)
    assert sha(dest)==sha(p)
    dest.chmod(stat.S_IREAD);backup['files'][fn]=dict(bytes=dest.stat().st_size,sha256=sha(dest),read_only=True)
dump(OUT/'recording-backup.json',backup)

def mean_table(mode,labels):
    lines=['| 対象 | A 平均 [範囲] | B 平均 [範囲] | B−A | 変化 | samples A/B |','| --- | ---: | ---: | ---: | ---: | ---: |']
    for label in labels:
        r=focus['comparison'][mode][label];a=r['A'];b=r['B'];pct='—' if r['raw_percent'] is None else f"{r['raw_percent']:+.2f}%"
        lines.append(f"| `{label}` | {a['mean']:.6f} [{a['minimum']:.6f}, {a['maximum']:.6f}] | {b['mean']:.6f} [{b['minimum']:.6f}, {b['maximum']:.6f}] | {r['delta_ms_per_tick']:+.6f} | {pct} | {a['samples'] if a['samples'] is not None else '—'} / {b['samples'] if b['samples'] is not None else '—'} |")
    return '\n'.join(lines)
def repetitions():
    lines=['| 順序 | 版 | モード | simulation CPU ms/tick | tick実時間 ms/tick | StepImpl self | RetiredPlan inclusive | lookup→plan inclusive |',
           '| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |']
    for key,r in focus['runs'].items():
        m=r['focus'];lines.append(f"| {key} | {r['label']} | {r['mode']} | {r['simulation_cpu_ms_per_tick']:.6f} | {r['simulation_wall_ms_per_tick']:.6f} | {m['StepImpl self']['ms_per_tick']:.6f} | {m['RetiredPlan']['ms_per_tick']:.6f} | {m['HyperpathLookupCandidates -> PlanPrecomputed']['ms_per_tick']:.6f} |")
    return '\n'.join(lines)
report=f'''# f97bac71 / 6c11c67f Clang ThinLTO ABBA計測

A=`{comparison['A_commit']}`、B=`{comparison['B_commit']}`。同じ凍結済み`gc3833d807.sav`から、Legacy／HyperpathをそれぞれA→B→B→Aで各2,048 tick採取した（2026-10-19夜開始、JST）。
**採用8本すべて、終了canonical 24項目が前回f97bac71の同じモードおよび今回の最初のAと完全一致した。**
両版ともClangCL / LLD 23.1.3、ThinLTO、assert OFF、PGO OFF。同じコンパイラ・ビルド条件で製品変更を比較している。

時間は主にメインスレッドの`StateGameLoop`を含むSampledProfileのCPU ms/tick。平均は各版・各モード2本、範囲は実測最小～最大。samplesは2本合計。
`StepImpl self`はスタック末端が`PassengerPrecomputedTable::StepImpl`であるサンプルのみ。ほかの指定関数・経路はinclusiveで、同じスタックを1回だけ数える。親子のinclusiveを加算していない。

## 観測した差

Hyperpathの`StepImpl` selfは0.261657→0.012971 ms/tick（−95.04%）、inclusiveも0.406058→0.115625（−71.52%）へ減った。
ワーカーの`PrepareHyperpathFinish`は新たに0.278744 ms/tickあり、行の仕上げがメインからワーカーへ移ったという実装と整合する。
`RetiredPlan`は0.117426→0.080056（−31.82%）、その配下の`PlanPrecomputed`は0.093263→0.029286（−68.60%）へ減った。

一方、**`HyperpathLookupCandidates`→`PlanPrecomputed`は0.022467→0.021752（−3.18%）、92→89 samplesにとどまり、期待した短縮は今回確認できない。**
lookup全体のinclusiveは0.135028→0.153601（+13.75%）へ増えた。新しい`SharedHyperpathPlan`はinclusive 0.115740、self 0.054450 ms/tickを占める。
共有処理のどこが支配的かは、この関数集計だけでは断定しない。`PlanPrecomputed`全体では0.301379→0.213626（−29.12%）だが、これをlookup配下の改善と混同しない。

Hyperpathのsimulation CPU平均は23.265649→23.026597（−1.03%）、tick実時間は24.830950→24.175775（−2.64%）。
ただしCPUの実測範囲はA 22.340818～24.190479、B 22.513335～23.539860と重なり、LegacyのCPU平均も−1.19%だった。
**局所的な削減は確認できるが、tick全体の約1%を再現性のある高速化とは断定しない。**
Legacyの`RetiredPlan`は0.408696→0.437507（+7.05%）で、2つ目の共有による短縮は観測していない。

## Hyperpath

{mean_table('hyperpath',['StateGameLoop','StepImpl self','StepImpl inclusive','RetiredPlan','RetiredPlan -> PlanPrecomputed','HyperpathLookupCandidates','HyperpathLookupCandidates -> PlanPrecomputed','SharedHyperpathPlan','SharedHyperpathPlan self','PlanPrecomputed','TickPrecomputed','simulation_wall_ms_per_tick'])}

## Legacy

{mean_table('legacy',['StateGameLoop','StepImpl self','StepImpl inclusive','RetiredPlan','RetiredPlan -> PlanPrecomputed','HyperpathLookupCandidates','HyperpathLookupCandidates -> PlanPrecomputed','SharedHyperpathPlan','SharedHyperpathPlan self','PlanPrecomputed','TickPrecomputed','simulation_wall_ms_per_tick'])}

Legacyではtick間のHyperpathプラン共有が適用されない。`SharedHyperpathPlan`がnullptrを返す判定コストはあり得る。関数別の小差だけを共有による改善と解釈しない。

## 各本の生値とばらつき

{repetitions()}

各ラウンド内はLegacy→Hyperpath、ラウンドはA1→B1→B2→A2。すべて新規プロセス・同じ固定入力で開始した。
B2 Legacyの最初の試行は別チャットのninja/lld-linkと重なり、runnerと区間監査が不合格にしたため除外した。採用済み4本は維持し、その1本だけ新規ゲートで取り直して残りを継続した。rejection.jsonとchecks/rejectedに不採用理由を保存した。
ABBAで時間に対して直線的なドリフトを釣り合わせるが、全てのノイズを除去するものではない。2回ずつの範囲は信頼区間ではない。untouchedによる追加補正は行っていない。
両版ともWPAで`StepHyperpath`の独立したフレームは観測しておらず、指定された`StepImpl`の同じself境界を比較した。0サンプルの経路は処理時間0の証明ではない。

## ワーカーと診断値

parallel rowsは終了時の現行generationの累積値であり、2,048 tick区間の差分カウンターではない。

`PrepareHyperpathFinish`はワーカー側を別集計した。これはメインスレッドのinclusiveと足していない。ワーカーへの処理移動によるメインCPU削減と、全スレッドCPU削減を区別する。

{mean_table('hyperpath',['worker PrepareHyperpathFinish','all_threads_cpu_ms_per_tick'])}

| 実行 | parallel rows（cargoごと） | lookup rebuilds | indexed_groups |
| --- | --- | ---: | ---: |
'''
for key,r in captures.items():
    lookup=r['diagnostics']['lookup'];parallel='<br>'.join(r['diagnostics']['parallel_rows']) or '旧版はこの行なし'
    report+=f"| {key} | {parallel} | {lookup['rebuilds']:,} | {lookup['indexed_groups']:,} |\n"
report+=f'''
## 条件・品質・保管

- 固定入力46,620,248 bytes、SHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットも前回manifestに照合。StateTicks **3,197,276,448 → 3,197,278,496**。
- `--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。replay・warm・背景処理が完了し、準備中tick不変。追加warm-up tick・cold・再構築指定なし。
- `openttd_lib`／exe／testの実コンパイルコマンドで`-flto=thin /O2 /Ob2 /MT /Z7`、`NDEBUG`、PGO指定なしを確認。静的CRT・compiler-rt builtins、`/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO`。元のmanifestを`mt.exe`で再埋め込みし、抽出内容を照合。
- Aは前回の独立保存済みexe/PDBをSHA-256照合して再利用。Bは対象commitの製品ソースから同じレシピでビルドし、全{tests[0][0]}テスト・{int(tests[0][1]):,} assertions成功。計測フックはコピーTUのみ。`StepImpl`・`RetiredPlan`・`HyperpathLookupCandidates`・`PlanPrecomputed`を対応PDBで事前確認。
- 同じkernel CPU専用File profile、1ms、1,024×1,024KiB。pilot成功後に採取。全8本でevents/buffers欠落0、両マーカーあり、kernel区間監査で競合ビルド・テスト・ゲームなし。
- WPA製品関数の未解決0。圧縮CSVと生CSVのハッシュ、exclusive時間・サンプル保存則、支払い整合性を確認。InvalidEvent警告はすべて採取マーカー外。
- canonicalは64bit digestを含めPython整数で24項目を比較。通常indexの性能計測であり、validateの成功判定は含まない。
- 生ETL8本・終了セーブ・Bのexe/PDBを作業ツリー外へ独立コピー。Aの既存独立バックアップも照合した。固定入力は既存ZIPを再利用。[保管manifest](measurements/{NAME}/recording-backup.json)、[入力ZIP](measurements/{NAME}/input-backup.json)。

[比較JSON](measurements/{NAME}/comparison.json)、[指定関数の各本CSV](measurements/{NAME}/focus-runs.csv)、[全関数のA/B平均差CSV](measurements/{NAME}/function-before-after.csv)、[canonical](measurements/{NAME}/canonical-comparison.json)、[採用検証](measurements/{NAME}/validation.json)。

各ラウンドの`legacy-functions.csv`／`hyperpath-functions.csv`にinclusive/exclusive、各モード配下にgzipの生WPA CSVを保存した。checksは当日のパス・版を含む再現資料であり、その場所から実行する汎用スクリプトではない。
'''
path=ROOT/'docs'/(NAME+'.md');path.write_text(report,encoding='utf-8',newline='\n')
dump(OUT/'report-sha256.json',dict(path=path.relative_to(ROOT).as_posix(),sha256=sha(path)))
dump(OUT/'evidence-sha256.json',{p.relative_to(OUT).as_posix():dict(bytes=p.stat().st_size,sha256=sha(p)) for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='evidence-sha256.json'})
(WORK/'evidence-complete.txt').write_text('Eight traces, backups and report complete\n',encoding='utf-8',newline='\n')
print(path,flush=True)
