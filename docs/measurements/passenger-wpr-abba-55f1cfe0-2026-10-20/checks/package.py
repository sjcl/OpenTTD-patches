"""Verify eight ABBA traces plus one validation run, preserve recordings, and write results."""
from pathlib import Path
import gzip
import hashlib
import json
import re
import shutil
import stat

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-abba-55f1cfe0-2026-10-20'
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-abba-6c11c67f-2026-10-19'
COMPILE=ROOT/'build-wpr-clang-thin-2026-10-19/compile'
INPUT=ROOT/'build-wpr-clang-thin-2026-10-19/input'
BACKUP=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329')/'passenger-wpr-recordings-abba-55f1cfe0-2026-10-20'
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
        copy(WORK/'sampling-jobs'/f'{counter:02}-{name}-{mode}'/'quality.json',dest/'recorder-quality.json')
        assert all(x['equal'] for x in captures[key]['canonical'].values())
        validation[key]=dict(exact_ticks=2048,canonical=captures[key]['canonical'],window_valid=True,lost_events=0,lost_buffers=0,
            raw_csv_hashes_match=True,unresolved_product_functions={},invalid_event_times_us=warnings,invalid_events_outside_markers=True,
            conservation_checks=s['conservation_checks'],payments_valid=True)
v=load(WORK/'validate-result.json');assert v['passed'] and v['canonical_equal'] and v['counters_passed']
for fn in ['process.json','actual-ticks.json','save-integrity.json','on-1-console.txt']:copy(WORK/'validate'/fn,OUT/'validate'/fn)
for fn in ['validate-result.json','validate-controller.log','validate-runner.log']:copy(WORK/fn,OUT/fn)
tests=re.findall(r'Passed all (\d+) test cases with (\d+) assertions\.',(WORK/'native-all.log').read_text(errors='replace'));assert len(tests)==1
dump(OUT/'validation.json',dict(A_commit=comparison['A_commit'],B_commit=comparison['B_commit'],accepted_traces=8,native_tests_B=int(tests[0][0]),
    native_assertions_B=int(tests[0][1]),same_input=True,same_capture_profile=True,runs=validation,real_map_validate=v))
for fn in ['prepare.py','estimated_prepare_compat.py','record.ps1','measure.py','post.py','analyze.py','package.py','verify.py','probe.py','check_build.py','validate.py']:
    copy(WORK/fn,OUT/'checks'/fn)
for fn in ['build-command.json','build-identity.json','baseline-identity.json','input-identity.json','prepare-compat.json','capture-results.json','plan.json',
    'capture-tool-identity.json','profile-interval.txt','recorder.log','native-all.log','configure.log','build.log','cpu-minimal.wprp','cpu-sampled.wpaProfile',
    'export-runner.log','project-flags.json','lto-objects.json','manifest-verification.json','manifest.log','openttd.exe.manifest.xml',
    'openttd_test.exe.manifest.xml','pdb-scope-check.txt','pdb-preflight.json','actual-compile-flags.json']:
    copy(WORK/fn,OUT/fn)
for target in ['openttd_lib','openttd','openttd_test']:
    p=next((COMPILE/(target+'.dir')/'Release').rglob('clang-cl.command.1.tlog'))
    (OUT/'checks'/(target+'-clang-command.tlog.gz')).write_bytes(gzip.compress(p.read_bytes(),mtime=0))
for fn in ['package.json','openttd.cfg']:copy(INPUT/fn,OUT/'input'/fn)
copy(BASE/'input-backup.json',OUT/'input-backup.json')
for fn in ['quality.json','job.json']:copy(WORK/'sampling-jobs/00-pilot'/fn,OUT/'checks/pilot'/fn)
for fn in ['recorder-markers.txt','recorder-trace-stats.txt']:copy(WORK/'pilot'/fn,OUT/'checks/pilot'/fn)
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/*markers.txt -whitespace\n**/*trace-stats.txt -whitespace\n',encoding='utf-8',newline='\n')
baseline_backup=load(BASE/'recording-backup.json');baseline_files={}
for fn in ['Release/openttd.exe','Release/openttd.pdb']:
    p=Path(baseline_backup['path'])/fn;item=baseline_backup['files'][fn]
    assert sha(p)==item['sha256'];baseline_files[fn]=item
BACKUP.mkdir(exist_ok=True)
backup=dict(path=str(BACKUP),files={},accepted_traces=8,validate_runs=1,profile_sha256=sha(WORK/'cpu-minimal.wprp'),
    A_existing_independent_backup=dict(path=baseline_backup['path'],files=baseline_files))
names=['Release/openttd.exe','Release/openttd.pdb','build-identity.json','validate/result.sav']+[name+'/'+mode+'/'+fn for name in ROUNDS for mode in ['legacy','hyperpath'] for fn in ['cpu.etl','result.sav']]
for fn in names:
    p=WORK/fn;dest=BACKUP/fn
    if not dest.exists():copy(p,dest)
    assert sha(dest)==sha(p);dest.chmod(stat.S_IREAD)
    backup['files'][fn]=dict(bytes=dest.stat().st_size,sha256=sha(dest),read_only=True)
dump(OUT/'recording-backup.json',backup)
def mean_table(mode,labels):
    lines=['| 対象 | A 平均 [範囲] | B 平均 [範囲] | B−A | 変化 | samples A/B |','| --- | ---: | ---: | ---: | ---: | ---: |']
    for label in labels:
        r=focus['comparison'][mode][label];a=r['A'];b=r['B'];pct='—' if r['raw_percent'] is None else f"{r['raw_percent']:+.2f}%"
        samples='—' if a['samples'] is None else f"{a['samples']:,} / {b['samples']:,}"
        lines.append(f"| `{label}` | {a['mean']:.6f} [{a['minimum']:.6f}, {a['maximum']:.6f}] | {b['mean']:.6f} [{b['minimum']:.6f}, {b['maximum']:.6f}] | {r['delta_ms_per_tick']:+.6f} | {pct} | {samples} |")
    return '\n'.join(lines)
def difference(mode,label):
    r=focus['comparison'][mode][label]
    return f"{r['A']['mean']:.6f} → {r['B']['mean']:.6f} ms/tick（{r['raw_percent']:+.2f}%）" if r['raw_percent'] is not None else f"{r['A']['mean']:.6f} → {r['B']['mean']:.6f} ms/tick"
labels=['StateGameLoop','simulation_wall_ms_per_tick','MayUseHyperpathPolicy','MayUseHyperpathPolicy self','ResolveHyperpath','BoardHyperpath','Board',
    'ResolveHyperpath / Board union','SharedHyperpathPlan self','SharedHyperpathPlan','RetiredPlan','PlanPrecomputed','StepImpl self','TickPrecomputed']
memo=v['diagnostics']['memo'];lookup=v['diagnostics']['lookup']
report=f'''# 6c11c67f / 55f1cfe0 Clang ThinLTO ABBA・validate計測

A=`{comparison['A_commit']}`、B=`{comparison['B_commit']}`。2026-10-20 JST、同じ凍結済み`gc3833d807.sav`からLegacy／Hyperpathを各A→B→B→A、計8本、各2,048 tick採取した。
**終了canonical 24項目は8本すべて6c11c67fの同じモードと完全一致。先行したB Hyperpath validateも完全一致し、memo・lookupのviolations=0、lookup_duplicates=0だった。**
0d99a67fで観測したcanonical不一致は、この修正版・入力・区間では再発しなかった。決定性や同期の全条件をこの1マップだけで証明するものではない。

両版ともClangCL/LLD 23.1.3、ThinLTO、assert OFF、PGO OFF、`/O2 /Ob2 /MT /Z7`、compiler-rt builtins、同じmanifestをmt.exeで再埋め込みした。
時間は主にメインスレッドの`StateGameLoop`を含むSampledProfileのCPU ms/tick。各版2本の平均・実測最小～最大と合計samplesを示す。tick実時間だけはrunnerのwall timer。

## 指定箇所と全体

- Hyperpath `MayUseHyperpathPolicy` inclusive: {difference('hyperpath','MayUseHyperpathPolicy')}。
- Hyperpath `ResolveHyperpath / Board union` inclusive: {difference('hyperpath','ResolveHyperpath / Board union')}。
- Hyperpath `SharedHyperpathPlan` self: {difference('hyperpath','SharedHyperpathPlan self')}。
- simulation CPU全体: Hyperpath {difference('hyperpath','StateGameLoop')}、Legacy {difference('legacy','StateGameLoop')}。

`MayUseHyperpathPolicy`はA 0.493533～0.613192、B 0.244097～0.272897 ms/tick、`SharedHyperpathPlan` selfはA 0.044921～0.058569、B 0.001954～0.002931で、今回の2本ずつではどちらも範囲が重ならず減少した。
`Resolve / Board union`はA 0.204558～0.256813、B 0.204067～0.236779と重なる。`Board`単体は0.191381→0.156959（−17.99%）だが、union全体の約4.45%を明確な短縮とは断定しない。
Aでは`BoardHyperpath`の独立したフレームが観測されず、その行の0は新規コストとの差を意味しない。親を含むunionを比較する。

**全体CPUはHyperpath −9.45%、Legacy −10.20%という生値だが、最適化単独の効果とは断定できない。**
同じA Hyperpathの実測が15.881876→22.560490 ms/tickと大きく変動し、A/Bの範囲も重なった。ABBAでも今回の変動を十分に除けたとは言えず、局所的な削減とtick全体の改善を区別する。

`Resolve / Board union`は`ResolveHyperpath`・`PassengerServiceDatabase::BoardHyperpath`・`PassengerPrecomputedTable::Board`のいずれかを含むスタックの和集合。親子のinclusiveを重複加算していない。
selfはスタック末端だけ。LTOやICFで境界が変わるため、0サンプルは処理時間0の証明ではない。版ごとの生値・範囲とサンプル数を優先し、各2本だけで小さな差の再現性を断定しない。untouched補正は行っていない。

## Hyperpath

{mean_table('hyperpath',labels)}

## Legacy

{mean_table('legacy',labels)}

LegacyにはHyperpath専用のtick間共有が適用されないため、同じ短縮を期待する対照として扱っていない。

## 各本の生値

| 順序 | 版 | モード | simulation CPU | tick実時間 | MayUse inclusive | Resolve/Board union | Shared self |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
'''
for key,r in focus['runs'].items():
    m=r['focus'];report+=f"| {key} | {r['label']} | {r['mode']} | {r['simulation_cpu_ms_per_tick']:.6f} | {r['simulation_wall_ms_per_tick']:.6f} | {m['MayUseHyperpathPolicy']['ms_per_tick']:.6f} | {m['ResolveHyperpath / Board union']['ms_per_tick']:.6f} | {m['SharedHyperpathPlan self']['ms_per_tick']:.6f} |\n"
report+=f'''
各ラウンド内はLegacy→Hyperpath、ラウンドはA1→B1→B2→A2。全て同じ入力から新規プロセスで開始した。ABBAは時間ドリフトの偏りを抑える順序であり、上の範囲は信頼区間ではない。

## validate（B Hyperpath、1本）

WPR前に同じ固定入力から`--hyperpath-filter-mode validate --validation-only`を1回実行した。性能の平均には含めていない。2,048 tickと準備完了・準備中tick不変を確認。

| 診断 | validated | conservative | violations | lookup_duplicates |
| --- | ---: | ---: | ---: | ---: |
| memo | {memo['validated']:,} | {memo['conservative']:,} | {memo['violations']} | {memo['lookup_duplicates']} |
| lookup | {lookup['validated_groups']:,} | — | {lookup['violations']} | — |

canonical 24項目は6c11c67f Hyperpathと完全一致。違反カウンター条件とcanonical条件の両方が合格した。`simulation_timing_valid=false`はvalidation-onlyの意図した値。

## 条件・採用検査・保管

- 固定入力46,620,248 bytes、SHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットも旧manifestに照合。StateTicks **3,197,276,448 → 3,197,278,496**。
- `--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。通常の準備だけで、追加warm-up tick・cold・再構築指定なし。validateだけ上記の別モード。
- Aは独立保存済み6c11c67f exe/PDBをハッシュ照合して再利用。Bは正確な55f1cfe0 checkoutから同じレシピでビルドした後、元の作業ブランチへ復帰。計測用フックはコピーTUだけに挿入した。
- Bの全{tests[0][0]} nativeテスト・{int(tests[0][1]):,} assertions成功。実clang-clコマンド、bitcode、compiler-rt builtins、抽出manifestと対応PDBを採取前に確認。
- 同じkernel CPU専用File profile、1ms、1,024×1,024KiB。pilot合格後に直列採取。採用8本すべてevents/buffers欠落0・両マーカーあり・kernel区間監査で競合ビルド／テスト／ゲームなし。関連チャットへ採取枠を事前調整した。
- WPA製品関数の未解決0。CSVのgzip展開ハッシュ、exclusive時間・サンプル保存則、支払い整合性を確認。InvalidEvent警告は測定マーカー外。
- ETL8本・終了セーブ9本・B exe/PDBを作業ツリー外へ独立コピー。Aの既存独立バックアップも再照合。固定入力ZIPは再利用。[保管manifest](measurements/{NAME}/recording-backup.json)、[入力ZIP](measurements/{NAME}/input-backup.json)。

[指定関数CSV](measurements/{NAME}/focus-runs.csv)、[全関数A/B差CSV](measurements/{NAME}/function-before-after.csv)、[canonical](measurements/{NAME}/canonical-comparison.json)、[validate](measurements/{NAME}/validate-result.json)、[採用検査](measurements/{NAME}/validation.json)、[比較JSON](measurements/{NAME}/comparison.json)。
各ラウンドの関数CSVにinclusive/exclusive、各モード配下のgzipに生WPA CSVを保存。checksは当日のパスを含む再現資料で、その場所から実行する汎用スクリプトではない。
'''
path=ROOT/'docs'/(NAME+'.md');path.write_text(report,encoding='utf-8',newline='\n')
dump(OUT/'report-sha256.json',dict(path=path.relative_to(ROOT).as_posix(),sha256=sha(path)))
dump(OUT/'evidence-sha256.json',{p.relative_to(OUT).as_posix():dict(bytes=p.stat().st_size,sha256=sha(p)) for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='evidence-sha256.json'})
(WORK/'evidence-complete.txt').write_text('Eight traces plus validation report preserved\n',encoding='utf-8',newline='\n')
print(path,flush=True)
