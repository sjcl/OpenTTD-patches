"""Verify exports, preserve raw recordings, and write the final measured report."""
from pathlib import Path
import gzip
import hashlib
import json
import re
import shutil
import stat

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-loading-scratch-8f829b8c-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
BACKUP=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329')/'passenger-wpr-recordings-8f829b8c-2026-10-19'

def load(path):return json.loads(path.read_text(encoding='utf-8-sig'))
def dump(path,value):path.write_text(json.dumps(value,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def copy(source,target):target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)

assert (WORK/'analysis-complete.txt').is_file()
summary=load(OUT/'summary.json');old_summary=load(BASE/'summary.json');comparison=load(OUT/'comparison.json')
focus=load(OUT/'focus.json');control=load(OUT/'control-0b8a21c0.json')
captures=comparison['capture_results'];validation={}
for index,mode in enumerate(['legacy','hyperpath'],1):
    s=summary[mode];assert s['lost_events']==s['lost_buffers']==0 and not s['unresolved_product_functions']
    assert s['payments_valid'] and s['ticks']==2048
    audit=load(WORK/mode/'window-audit.json');assert audit['valid']
    warnings=[int(t) for warning in s['wpa_invalid_event_warning'] for t in re.findall(r'TimeStamp:\s*(\d+)',warning)]
    assert all(t<audit['marker_begin_us'] or t>audit['marker_end_us'] for t in warnings)
    exported=load(OUT/mode/'export-command.json')
    for name,item in exported['exports'].items():
        with gzip.open(OUT/mode/(name+'.gz'),'rb') as stream:assert hashlib.file_digest(stream,'sha256').hexdigest()==item['sha256']
    for name in ['window-audit.json','window-markers.txt','window-processes.txt','recorder-markers.txt','recorder-trace-stats.txt']:
        copy(WORK/mode/name,OUT/mode/name)
    copy(WORK/'sampling-jobs'/f'{index:02}-{mode}'/'quality.json',OUT/mode/'recorder-quality.json')
    validation[mode]=dict(exact_ticks=2048,canonical=captures[mode]['canonical'],window_valid=True,lost_events=0,lost_buffers=0,
        raw_csv_hashes_match=True,unresolved_product_functions={},invalid_event_times_us=warnings,invalid_events_outside_markers=True,
        conservation_checks=s['conservation_checks'],payments_valid=True)
    assert all(item['equal'] for item in captures[mode]['canonical'].values())
tests=re.findall(r'Passed all (\d+) test cases with (\d+) assertions\.',(WORK/'native-all.log').read_text(errors='replace'))
assert len(tests)==1
dump(OUT/'validation.json',dict(product_commit='8f829b8c3d5f35cb420c2a16812716ef24e38939',baseline_commit='0b8a21c0f3dc9a24ca0d9d3581c19fb9c207e684',
     accepted_traces=2,native_tests=int(tests[0][0]),native_assertions=int(tests[0][1]),same_input=True,same_capture_profile=True,runs=validation))
for name in ['prepare.py','estimated_prepare_compat.py','record.ps1','measure.py','post.py','analyze.py','package.py','verify.py','finish.py']:
    if (WORK/name).is_file():copy(WORK/name,OUT/'checks'/name)
for name in ['build-command.json','build-identity.json','input-identity.json','prepare-compat.json','capture-results.json','profile-interval.txt','recorder.log','native-all.log','configure.log','build.log','cpu-minimal.wprp','cpu-sampled.wpaProfile','export-runner.log']:
    copy(WORK/name,OUT/name)
for name in ['package.json','openttd.cfg']:copy(WORK/'input'/name,OUT/'input'/name)
copy(BASE/'input-backup.json',OUT/'input-backup.json')
for name in ['quality.json','job.json','rejected-start-job.json']:copy(WORK/'sampling-jobs/00-pilot'/name,OUT/'checks/pilot'/name)
for name in ['recorder-markers.txt','recorder-trace-stats.txt']:copy(WORK/'pilot-2'/name,OUT/'checks/pilot'/name)
copy(WORK/'pilot/wpr-error.txt',OUT/'checks/pilot/initial-start-error.txt')
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/*markers.txt -whitespace\n**/*trace-stats.txt -whitespace\n',encoding='utf-8',newline='\n')
BACKUP.mkdir()
backup=dict(path=str(BACKUP),files={},accepted_traces=2,profile_sha256=sha(WORK/'cpu-minimal.wprp'))
for name in ['Release/openttd.exe','Release/openttd.pdb','build-identity.json','legacy/cpu.etl','hyperpath/cpu.etl','legacy/result.sav','hyperpath/result.sav']:
    dest=BACKUP/name;copy(WORK/name,dest);assert sha(dest)==sha(WORK/name)
    dest.chmod(stat.S_IREAD);backup['files'][name]=dict(bytes=dest.stat().st_size,sha256=sha(dest),read_only=True)
dump(OUT/'recording-backup.json',backup)

labels=['StationCargoList::Load','Load -> _malloc_base','Load -> RtlFreeHeap','LoadingContext constructor','LoadingContext destructor',
        'Load -> LoadingContext constructor','Load -> LoadingContext destructor','MayUseHyperpathPolicy','MayUseHyperpathPolicy -> PlanPrecomputed','CanUseHyperpathPolicy -> PlanPrecomputed','PlanPrecomputed','loading walk']
def table(mode):
    lines=['| 関数 / 呼出し経路 | 0b8a21c0 | 8f829b8c 生値 | 生の変化 | 補正後 | samples 旧→新 |',
           '| --- | ---: | ---: | ---: | ---: | ---: |']
    for label in labels:
        r=focus[mode][label];a=r['old'];b=r['new'];pct='—' if r['raw_percent'] is None else f"{r['raw_percent']:+.2f}%"
        lines.append(f"| `{label}` | {a['ms_per_tick']:.6f} | {b['ms_per_tick']:.6f} | {pct} | {r['new_adjusted_ms_per_tick']:.6f} | {a['samples']:,} → {b['samples']:,} |")
    return '\n'.join(lines)
report=f'''# 8f829b8c: Passenger積み込み時のheap churn削減

同じ固定済み `gc3833d807.sav` を使い、`8f829b8c3d5f35cb420c2a16812716ef24e38939` で通常条件のLegacy・Hyperpathを各2,048 tick、WPRを各1本採取した（2026-10-19 JST）。
比較基準は同じkernel CPU専用プロファイルで採った0b8a21c0の保存済み2本。
**終了canonical 24項目は両モードとも97cdc0ad・0b8a21c0の同じモードと完全一致し、Hyperpathは6aeaff68のvalidateとも完全一致した。**
6aeaff68にはLegacyの記録がなく、時間のプロファイルもない。今回の時間差は6aeaff68と8f829b8cの両変更を含み、それぞれの単独効果へは分離していない。

HyperpathのLoad配下では、malloc/freeは補正後も減った。Load配下のコンストラクタ／デストラクタは今回0サンプルだった。一方、`MayUseHyperpathPolicy -> PlanPrecomputed` は生値では減ったが、補正後は前回を下回らなかった。
Hyperpathの未変更部分の新/旧比は約0.761と大きく変動した。全体のsampled CPU差は生値−6.527 ms/tick、補正後+0.264 ms/tickであり、この1本ずつからtick全体の高速化は確認できない。

時間はメインスレッドで `StateGameLoop` を含むサンプルのinclusive CPU ms/tick。`A -> B` は同じスタックでAの子孫にBがある経路だけを数えた。
1スタック内の重複は1回だけ数え、親子inclusiveを足していない。

## Hyperpath

{table('hyperpath')}

## Legacy

{table('legacy')}

コンストラクタ／デストラクタの表はシミュレーション全体のinclusive。Load配下だけの値もfocus.jsonに保存した。
loading walkはCargoLoad用VisitBoardingCandidates本体。`_malloc_base` / `RtlFreeHeap` はLoad配下だけであり、プロセス全体のheap合計ではない。
サンプル0は実行時間0の証明ではない。インライン化・ICFによるシンボル帰属差にも留意する。

## memoと全体時間

| モード | memo hits 0b8a21c0 → 8f829b8c | lookup rebuilds | indexed_groups |
| --- | ---: | ---: | ---: |
'''
for mode in ['legacy','hyperpath']:
    d=captures[mode];memo=d['diagnostics']['memo'];lookup=d['diagnostics']['lookup']
    report+=f"| {mode} | {d['baseline_memo']['hits']:,} → {memo['hits']:,} | {lookup['rebuilds']:,} | {lookup['indexed_groups']:,} |\n"
report+='\n通常indexモードの記録で、validateは追加実行していない。診断の生値はcapture-results.json / comparison.jsonとコンソール全文に保存した。\n\n'
report+='| モード | tick実時間 ms/tick 旧→新 | sampled CPU ms/tick 旧→新 | 補正係数 | 補正後CPU差 ms/tick |\n| --- | ---: | ---: | ---: | ---: |\n'
for mode in ['legacy','hyperpath']:
    a=old_summary[mode];b=summary[mode];c=control[mode]
    report+=f"| {mode} | {a['simulation_wall_ms_per_tick']:.6f} → {b['simulation_wall_ms_per_tick']:.6f} | {a['sampled_cpu_ms_per_tick']['simulation_stack']:.6f} → {b['sampled_cpu_ms_per_tick']['simulation_stack']:.6f} | {c['machine_factor']:.9f} | {c['corrected_delta_ms_per_tick']:+.6f} |\n"
report+=f'''
補正は `passenger_wpr_control.py` のuntouched新/旧比を使った参考値で、CPU周波数の直接測定ではない。生値を併記した。各組合せ1回のため測定ばらつきを含む。

## 条件・品質・保管

- 固定セーブSHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`、cfgと245アセットも前回manifestに照合。変換・再保存なし。
- StateTicks 3,197,276,448 → 3,197,278,496、実tick差・サンプル数とも各2,048。全canonicalをPython整数で照合。
- `--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。モードだけ切替。停止中replay・warm・背景準備が成功し、準備前後のtick不変。cold・再構築・追加warm-up tickなし。
- MSVC 19.44 x64 Release、静的CRT、assert ON、LTO OFF、前回と同じフラグとコピーTUの計測フック。製品コードへの追加変更なし。全{int(tests[0][0]):,} nativeテスト・{int(tests[0][1]):,} assertions成功。
- 短いpilotの後、Legacy→Hyperpathを直列採取。kernel CPU専用File方式、1ms、1,024×1,024KiBのsystem buffer。同一WPR/WPA profileをSHA-256で確認。
- 両本のlost events / buffers=0、両マーカーあり、kernel区間監査で競合ビルド・テスト・ゲームなし。製品関数の未解決なし。CSVハッシュと時間・サンプル保存則、支払い整合性を検証。WPAのInvalidEvent警告はマーカー外。
- 初回pilotのrecorder起動失敗は小さいログを保存。一時フォルダー事前作成とWindows PowerShellのstderr取得を修正し、短いpilotを再確認してから本番へ進んだ。本番の2本は各1回。既存のNewGRF警告とerror pause解除のログも保持。
- 採用ETL・exe/PDB・終了セーブは作業ツリー外に独立コピーし、recording-backup.jsonへ場所・容量・SHA-256を記録。固定入力は既存ZIPを再利用した。

[注目経路](measurements/{NAME}/focus.json)、[関数別inclusive/exclusive差分CSV](measurements/{NAME}/function-before-after.csv)、[canonical照合](measurements/{NAME}/canonical-comparison.json)、[補正計算](measurements/{NAME}/control-0b8a21c0.json)、[品質検証](measurements/{NAME}/validation.json)。
生WPA CSVは各モード配下のgzip。checks内のスクリプトは当日のパス・版を含む再現資料で、その場所から直接実行する汎用ツールではない。

補正の再計算:

```powershell
python -B utils/passenger_wpr_control.py docs/measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19 docs/measurements/{NAME}
```
'''
report_path=ROOT/'docs'/(NAME+'.md');report_path.write_text(report,encoding='utf-8',newline='\n')
dump(OUT/'report-sha256.json',dict(path=report_path.relative_to(ROOT).as_posix(),sha256=sha(report_path)))
dump(OUT/'evidence-sha256.json',{p.relative_to(OUT).as_posix():dict(bytes=p.stat().st_size,sha256=sha(p)) for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='evidence-sha256.json'})
(WORK/'evidence-complete.txt').write_text('Report, checks and recording backups complete\n',encoding='utf-8',newline='\n')
print('Evidence and report prepared: '+str(report_path),flush=True)
