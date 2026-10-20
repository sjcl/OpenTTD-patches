"""Archive the accepted captures and separate validation, then write raw ABBA tables."""
from pathlib import Path
import gzip, hashlib, json, re, shutil, stat

ROOT=Path('C:/Users/bebe/.codex/worktrees/3abe/OpenTTD-patches')
NAME='passenger-wpr-abba-0d99a67f-2026-10-20'
WORK=ROOT/('build-wpr-abba-0d99a67f-2026-10-20')
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-abba-6c11c67f-2026-10-19'
BACKUP=Path('C:/Users/bebe/.codex/visualizations/2026/10/19/01a15484-1ab3-7172-aee8-1a55c6d1c3b4')/('passenger-wpr-recordings-abba-0d99a67f-2026-10-20')
ROUNDS=['01-A1','02-B1','03-B2','04-A2']
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def dump(p,x):p.write_text(json.dumps(x,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def copy(a,b):b.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(a,b)

assert (WORK/'analysis-complete.txt').is_file()
captures=load(WORK/'capture-results.json');focus=load(OUT/'focus.json');comparison=load(OUT/'comparison.json')
validate=load(WORK/'validate-result.json');assert validate['passed']
validation={};counter=0
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
        copy(WORK/'sampling-jobs'/f'{counter:02}-{name}-{mode}'/'quality.json',dest/'recorder-quality.json')
        assert all(x['equal'] for x in captures[key]['canonical'].values())
        validation[key]=dict(exact_ticks=2048,canonical=captures[key]['canonical'],window_valid=True,lost_events=0,lost_buffers=0,
            raw_csv_hashes_match=True,unresolved_product_functions={},invalid_event_times_us=warnings,invalid_events_outside_markers=True,
            conservation_checks=s['conservation_checks'],payments_valid=True)
tests=re.findall(r'Passed all (\d+) test cases with (\d+) assertions\.',(WORK/'native-all.log').read_text(errors='replace'));assert len(tests)==1
dump(OUT/'validation.json',dict(A_commit=comparison['A_commit'],B_commit=comparison['B_commit'],accepted_traces=8,native_tests_B=int(tests[0][0]),
    native_assertions_B=int(tests[0][1]),same_input=True,same_capture_profile=True,runs=validation,separate_validate=validate))
for p in WORK.glob('*.py'):copy(p,OUT/'checks'/p.name)
copy(WORK/'record.ps1',OUT/'checks/record.ps1')
for fn in ['plan.json','build-command.json','build-identity.json','baseline-identity.json','input-identity.json','prepare-compat.json','capture-results.json',
    'capture-tool-identity.json','profile-interval.txt','recorder.log','native-all.log','configure.log','build.log','cpu-minimal.wprp','cpu-sampled.wpaProfile',
    'export-runner.log','project-flags.json','lto-objects.json','manifest-verification.json','manifest.log','openttd.exe.manifest.xml',
    'openttd_test.exe.manifest.xml','pdb-scope-check.txt','pdb-preflight.json','actual-compile-flags.json','validate-result.json','validate-runner.log']:
    copy(WORK/fn,OUT/fn)
for target in ['openttd_lib','openttd','openttd_test']:
    p=next((WORK/'compile'/(target+'.dir')/'Release').rglob('clang-cl.command.1.tlog'))
    (OUT/'checks'/(target+'-clang-command.tlog.gz')).write_bytes(gzip.compress(p.read_bytes(),mtime=0))
for fn in ['package.json','openttd.cfg']:copy(WORK/'input'/fn,OUT/'input'/fn)
input_backup=load(BASE/'input-backup.json');assert sha(Path(input_backup['path']))==input_backup['sha256']
copy(BASE/'input-backup.json',OUT/'input-backup.json')
for fn in ['quality.json','job.json']:copy(WORK/'sampling-jobs/00-pilot'/fn,OUT/'checks/pilot'/fn)
for fn in ['recorder-markers.txt','recorder-trace-stats.txt']:copy(WORK/'pilot'/fn,OUT/'checks/pilot'/fn)
for fn in ['process.json','actual-ticks.json','save-integrity.json','on-1-console.txt','control.txt','on.cfg']:
    if (WORK/'validate'/fn).exists():copy(WORK/'validate'/fn,OUT/'validate'/fn)
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/*markers.txt -whitespace\n**/*trace-stats.txt -whitespace\n',encoding='utf-8',newline='\n')
BACKUP.mkdir(exist_ok=False)
backup=dict(path=str(BACKUP),files={},accepted_traces=8,profile_sha256=sha(WORK/'cpu-minimal.wprp'))
names=['Release/openttd.exe','Release/openttd.pdb','build-identity.json','baseline/Release/openttd.exe','baseline/Release/openttd.pdb','baseline/build-identity.json',
       'validate/result.sav']+[name+'/'+mode+'/'+fn for name in ROUNDS for mode in ['legacy','hyperpath'] for fn in ['cpu.etl','result.sav']]
for fn in names:
    p=WORK/fn;dest=BACKUP/fn;copy(p,dest);assert sha(dest)==sha(p)
    dest.chmod(stat.S_IREAD);backup['files'][fn]=dict(bytes=dest.stat().st_size,sha256=sha(dest),read_only=True)
dump(OUT/'recording-backup.json',backup)

def mean_table(mode,labels):
    lines=['| 対象 | A 平均 [範囲] | B 平均 [範囲] | B−A | 変化 | samples A/B |','| --- | ---: | ---: | ---: | ---: | ---: |']
    for label in labels:
        r=focus['comparison'][mode][label];a=r['A'];b=r['B'];pct='—' if r['raw_percent'] is None else f"{r['raw_percent']:+.2f}%"
        lines.append(f"| `{label}` | {a['mean']:.6f} [{a['minimum']:.6f}, {a['maximum']:.6f}] | {b['mean']:.6f} [{b['minimum']:.6f}, {b['maximum']:.6f}] | {r['delta_ms_per_tick']:+.6f} | {pct} | {a['samples'] if a['samples'] is not None else '—'} / {b['samples'] if b['samples'] is not None else '—'} |")
    return '\n'.join(lines)
def repetitions():
    lines=['| 順序 | 版 | モード | simulation CPU ms/tick | tick実時間 ms/tick | MayUse inclusive | Resolve inclusive | Board inclusive | Shared self |',
           '| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for key,r in focus['runs'].items():
        m=r['focus'];lines.append(f"| {key} | {r['label']} | {r['mode']} | {r['simulation_cpu_ms_per_tick']:.6f} | {r['simulation_wall_ms_per_tick']:.6f} | {m['MayUseHyperpathPolicy']['ms_per_tick']:.6f} | {m['ResolveHyperpath']['ms_per_tick']:.6f} | {m['Board']['ms_per_tick']:.6f} | {m['SharedHyperpathPlan self']['ms_per_tick']:.6f} |")
    return '\n'.join(lines)
d=validate['diagnostics']
report=f'''# 6c11c67f / 0d99a67f Clang ThinLTO ABBA計測

A=`{comparison['A_commit']}`、B=`{comparison['B_commit']}`。同じ凍結済み`gc3833d807.sav`から、Legacy／HyperpathをそれぞれA→B→B→A、各2,048 tick採取した（2026-10-20 JST）。
**採用8本と別枠validate 1本でcanonical 24項目が一致。validateはmemo・lookupともviolations=0、lookup_duplicates=0で合格した。**

両版ともClangCL / LLD 23.1.3、ThinLTO、assert OFF、PGO OFF、静的CRT。Aは前回の独立保存済みexe/PDBをSHA-256照合し、Bを同条件でビルドした。
全{tests[0][0]} native tests、{int(tests[0][1]):,} assertionsが成功。製品ソースは対象commitのまま、計測フックはコピーTUだけに挿入した。

## 指定関数（Hyperpath）

メインスレッドの`StateGameLoop`を含むSampledProfileのCPU ms/tick。平均は各版2本、範囲は実測最小～最大、samplesは2本合計。
`SharedHyperpathPlan self`はスタック末端だけ。ほかはinclusive。同一スタック内の同じ関数は1回だけ数え、親子のinclusiveは加算しない。
`ResolveHyperpath / Board union`はResolveHyperpath、BoardHyperpath、PassengerPrecomputedTable::Boardのいずれかを含むスタックの和集合で、重複しない。

{mean_table('hyperpath',['MayUseHyperpathPolicy','MayUseHyperpathPolicy self','ResolveHyperpath','BoardHyperpath','Board','ResolveHyperpath / Board union','SharedHyperpathPlan self','SharedHyperpathPlan','StateGameLoop','simulation_wall_ms_per_tick','all_threads_cpu_ms_per_tick'])}

## Legacyと周辺処理

{mean_table('legacy',['MayUseHyperpathPolicy','ResolveHyperpath','Board','SharedHyperpathPlan self','StateGameLoop','simulation_wall_ms_per_tick'])}

Hyperpathの周辺処理:

{mean_table('hyperpath',['StepImpl self','RetiredPlan','HyperpathLookupCandidates','HyperpathLookupCandidates -> PlanPrecomputed','PlanPrecomputed','TickPrecomputed','worker PrepareHyperpathFinish'])}

## 全8本の生値

{repetitions()}

各ラウンド内はLegacy→Hyperpath。全8本とも新規プロセス・同じ固定入力。ABBAは直線的な時間ドリフトを釣り合わせるが、ノイズを除去するものではない。
2回ずつの範囲は信頼区間ではない。untouchedによる機械補正は行わない。LTOで消えた関数の0サンプルは時間0の証明ではない。

## 別枠validate（B、Hyperpath、1本）

同じ固定入力・2,048 tick、`--hyperpath-filter-mode validate --validation-only`。性能平均には含めない。
保存済みreplay・warm・背景処理が完了し、準備中tick不変。開始3197276448、終了3197278496。終了canonicalはA1 Hyperpathの24項目と完全一致。
`simulation_timing_valid=false`はvalidation-onlyの意図した値。

| 診断 | validated | violations | lookup_duplicates |
| --- | ---: | ---: | ---: |
| memo | {d['memo']['validated']:,} | {d['memo']['violations']} | {d['memo']['lookup_duplicates']} |
| lookup | {d['lookup']['validated_groups']:,} | {d['lookup']['violations']} | — |

[validate判定・実行レシピ](measurements/{NAME}/validate-result.json)、[全診断ログ](measurements/{NAME}/validate/on-1-console.txt)。canonical一致とvalidate合格を独立して確認した。

## 条件・品質・保管

- 固定入力46,620,248 bytes、SHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットも前回manifestと一致。StateTicks **3,197,276,448 → 3,197,278,496**。
- `--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。replay・warm・背景処理完了、準備中tick不変。追加warm-up tick・cold・再構築指定なし。
- 実コンパイルコマンドで`-flto=thin /O2 /Ob2 /MT /Z7`、`NDEBUG`、PGOなしを確認。bitcodeオブジェクトとcompiler-rt builtinsの実リンク先・ハッシュを記録。元manifestを再埋め込みして抽出内容を照合。対応PDBの内部関数名を事前確認。
- kernel CPU専用File profile、1ms、1,024×1,024KiB。pilot成功後に採取。全8本でevents/buffers欠落0、両マーカーあり、kernel区間監査で競合ビルド・テスト・ゲームなし。
- WPA製品関数の未解決0。圧縮CSVと生CSVのハッシュ、exclusive時間・サンプル保存則、支払い整合性を確認。InvalidEvent警告はすべてマーカー外。
- canonicalは64bit digestを含めPython整数で24項目を比較し、前回6c11c67fの同モードと今回A1に照合した。
- 生ETL8本・終了セーブ9本・A/B exe/PDBを作業ツリー外へ独立コピーして読み取り専用化。固定入力は既存ZIPのハッシュを再照合して保管を確認。[保管manifest](measurements/{NAME}/recording-backup.json)、[入力ZIP](measurements/{NAME}/input-backup.json)。

[比較JSON](measurements/{NAME}/comparison.json)、[指定関数の各本CSV](measurements/{NAME}/focus-runs.csv)、[全関数の平均差CSV](measurements/{NAME}/function-before-after.csv)、[canonical](measurements/{NAME}/canonical-comparison.json)、[採用検証](measurements/{NAME}/validation.json)。

各ラウンドの関数別CSVにinclusive/exclusive、各モード配下にgzipの生WPA CSVを保存。checksは当日の固定パスを含む再現資料であり、その場所から実行する汎用スクリプトではない。
'''
path=ROOT/'docs'/(NAME+'.md');path.write_text(report,encoding='utf-8',newline='\n')
dump(OUT/'report-sha256.json',dict(path=path.relative_to(ROOT).as_posix(),sha256=sha(path)))
dump(OUT/'evidence-sha256.json',{p.relative_to(OUT).as_posix():dict(bytes=p.stat().st_size,sha256=sha(p)) for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='evidence-sha256.json'})
(WORK/'evidence-complete.txt').write_text('Eight accepted traces, one validation, backups and report complete\n',encoding='utf-8',newline='\n')
print(path,flush=True)
