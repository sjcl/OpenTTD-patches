"""Render requested canonical and LocalPolicy comparisons, plus prior-results cleanup."""
from pathlib import Path
import gzip
import json
import re

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-local-policy-9ab0228d-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-generation-f9c4a85b-2026-10-18'
REPORT=ROOT/'docs'/(NAME+'.md')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def ms(x):return f'{x:.6f}'
def pct(x):return f'{x:+.2f}%'
focus=load(OUT/'focus.json');canonical=load(OUT/'canonical-comparison.json');control=load(OUT/'machine-control.json')
summary=load(OUT/'summary.json');old_summary=load(BASE/'summary.json');proof=load(OUT/'validation.json')
cleanup=load(OUT/'previous-results-cleanup.json')
local='openttd.exe!PassengerPrecomputedTable::LocalPolicy'
can='openttd.exe!PassengerJourneyLoadingContext::CanUseHyperpathPolicy'
station='openttd.exe!PassengerPrecomputedTable::StationChoices'
lines=['# 9ab0228d: LocalPolicy共有キャッシュの実マップWPR','',
'`9ab0228db17178ef931de961305ebec544162f6a` で、前回と同じ固定済み `gc3833d807.sav` からLegacyとHyperpathを各2,048 tick、有効な記録を1本ずつ取得した。',
'計測は2026-10-18に開始し、10-19に終了した（JST）。比較対象は前回のf9c4a85b。','']
if all(x['equal'] for x in canonical.values()):
    lines+=['**両モードとも、終了canonical 24項目すべてが前回の同じモードと完全一致した。**',
        '64 bit digestもPythonの整数として比較し、浮動小数点へ変換していない。','']
else:
    lines+=['**終了canonicalが一致しないモードがある。時間差を結果不変の高速化として解釈しない。**',
        '`canonical-comparison.json` に全差分を保存した。','']
lines+=['| モード | 前回とのcanonical一致 | 項目数 |',
'| --- | --- | ---: |']
for mode in ['legacy','hyperpath']:
    lines.append(f"| {mode} | {'完全一致' if canonical[mode]['equal'] else '不一致'} | {canonical[mode]['fields']} |")
lines+=['','CPU Usage (Sampled)の `StateGameLoop` を含むスタックが対象。各スタック内で同じ関数を1回だけ数えたinclusive。',
'表の親子関数のinclusiveは加算しない。単位はCPU ms/tick。','',
'| モード / 関数 | f9c4a85b | 9ab0228d 生値 | 生の変化 | 9ab0228d 補正後 | 補正後の変化 |',
'| --- | ---: | ---: | ---: | ---: | ---: |']
for mode in ['hyperpath','legacy']:
    for name,label in [(local,'LocalPolicy'),(can,'CanUseHyperpathPolicy')]:
        f=focus[mode][name]
        lines.append(f"| {mode} / `{label}` | {ms(f['inclusive_old_ms_per_tick'])} | {ms(f['inclusive_new_ms_per_tick'])} | {pct(f['inclusive_raw_percent'])} | {ms(f['inclusive_new_adjusted_ms_per_tick'])} | {pct(f['inclusive_adjusted_percent'])} |")
lines+=['','`CanUseHyperpathPolicy` は `PassengerJourneyLoadingContext` のメソッド。',
'新しい `StationChoices` は `LocalPolicy` から呼ばれるため、そのコストも上のLocalPolicy inclusiveに含まれる。',
'StationChoicesへ移した処理を計測対象から外していない。','',
'| モード | LocalPolicy前回 → 今回 samples | CanUseHyperpathPolicy前回 → 今回 samples | 新StationChoices inclusive CPU ms/tick / samples |',
'| --- | ---: | ---: | ---: |']
for mode in ['hyperpath','legacy']:
    a=focus[mode][local];b=focus[mode][can];c=focus[mode].get(station,{})
    lines.append(f"| {mode} | {a['inclusive_old_samples']:,} → {a['inclusive_new_samples']:,} | {b['inclusive_old_samples']:,} → {b['inclusive_new_samples']:,} | {ms(c.get('inclusive_new_ms_per_tick',0))} / {c.get('inclusive_new_samples',0):,} |")
lines+=['','補正には `utils/passenger_wpr_control.py` の既定分類を用いた。',
'untouched時間の比（新/旧）を共通の機械速度変動と仮定し、新版時間をその比で割った。CPU周波数の直接測定ではない。',
'各モード1回のサンプリング測定であり、小さい差には測定ばらつきが含まれる。','',
'| モード | 補正係数 新/旧 | tick全体 実時間 ms/tick 前回 → 今回 |',
'| --- | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    lines.append(f"| {mode} | {control[mode]['machine_factor']:.9f} | {ms(old_summary[mode]['simulation_wall_ms_per_tick'])} → {ms(summary[mode]['simulation_wall_ms_per_tick'])} |")
lines+=['','計測条件・検証:','',
'- 同じセーブ・設定・245個のアセットをSHA-256で確認。StateTicks 3,197,276,448 → 3,197,278,496。各2,048個の実tickサンプル。',
'- 前回と同じ順序（Legacy、Hyperpath）、`--resume --precomputed --warm-precomputed --wait-background-ready`、Hyperpath索引ON、メインスレッドaffinity mask=1。',
'- 停止中の準備でtickが進んでいないことを確認。再構築・cold条件・追加トレース・新たな定常化なし。',
'- MSVC 19.44 x64 Release、`/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert ON、LTO OFF。CPU採取項目・スタック設定とWPAツールは前回と同じ。Hyperpathの記録方式は下記のとおり変更した。',
f"- 全{proof['native_tests']:,} nativeテスト・{proof['native_assertions']:,} assertionsが通った。製品ソースへの追加変更なし。",
'- 両区間のkernel監査で競合ビルド・テスト・ゲームなし。lost events / buffersは0、製品関数の未解決シンボルなし。',
'- 生CSVのSHA-256、サンプル数・exclusive時間の保存則、支払い整合性、WPA警告が計測マーカー外にあることを検証。','',
'Hyperpathはメモリ方式の3回で開始マーカーがなく、性能集計から除外した。1秒の準備待ちとイベントバッファ20→256への拡大も試したが、改善しなかった。canonical 24項目は3回とも一致していた。',
'採用するHyperpath記録は同じ入力・バイナリで `wpr -start CPU -filemode` を使った。開始前の計測区間外に1秒の待ちと準備マーカーを入れた。',
'**Hyperpathの時間比較は、前回のMemoryと今回のFileという保存方式・バッファ構成の差を含む。** Legacyは前回と同じMemory方式。',
'CPUプロファイルをエクスポートし、SystemProviderのkeywords・stacks、EventProvider群、各collectorのprovider参照が一致することをXMLで検証した。system buffer設定はMemory 4,781→File 20、event buffer設定は20→20。',
'ファイル方式とプロファイルのエクスポートは[MicrosoftのWPR CLI仕様](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options)に従った。',
'設定と一致確認は `recorder-settings.json`、`checks/cpu-memory.wprp` と `checks/cpu-file.wprp`。開始・終了マーカー両方の監査に通った記録だけを採用した。',
'不採用3回のマーカー表・ログ・プロセスメタデータは `checks/rejected-missing-begin-marker*/` に残し、大きな不採用ETLと重複入力は削除した。','',
'前回同様、JP+ Multiple Unitsの車両容量変更に関するNewGRF警告が出るセーブで計測した。警告を抑制する変更はしていない。','']
for mode in ['legacy','hyperpath']:
    now=(WORK/mode/'stderr.txt').read_text(encoding='utf-8-sig',errors='replace')
    before=gzip.decompress((BASE/mode/'stderr.txt.gz').read_bytes()).decode('utf-8-sig',errors='replace')
    count=lambda t:len(re.findall(r"Behaviour of NewGRF .* is likely to cause desyncs and/or crashes",t))
    resumes=(WORK/mode/'on-1-console.txt').read_text(encoding='utf-8-sig').count('Measurement resumed a NewGRF error pause.')
    lines.append(f'- {mode}: NewGRF警告 前回 {count(before):,}件 → 今回 {count(now):,}件、今回の計測用フックによるerror pause解除 {resumes}回。')
lines+=['','過去の未追跡データの削除:','',
'ユーザーの追加依頼に従い、過去5つの未追跡作業フォルダーと作業ツリー外の旧記録・入力バックアップ4件を削除した。',
'Gitに保存された報告書・CSV・canonical・ソース、指定の元セーブ、今回の9ab0228d計測は残した。',
'必要なWPAツールは今回の作業フォルダーへ移してから削除した。削除処理は2本のWPR記録・保存終了後に実行した。',
f"削除前後のCドライブ空き容量は {cleanup['free_before']/1e9:.2f} GB → {cleanup['free_after']/1e9:.2f} GB。",
'対象パス・件数・検証結果は `previous-results-cleanup.json`。別途、今回の準備中に重複シンボルキャッシュ約16.6 GBも整理した。',
'**過去の報告書に記載された作業ツリー外の旧ETL・旧入力バックアップは、今回の依頼で削除済み。** Git内のCSVとcanonicalは引き続き参照できる。',
'今回分だけはETL・対応exe/PDB・固定入力のZIPを作業ツリー外にも保持し、`recording-backup.json` と `input-backup.json` に場所とSHA-256を記録した。','',
f'[関数別inclusive/exclusive差分CSV](measurements/{NAME}/function-before-after.csv)、',
f'[注目関数の全数値](measurements/{NAME}/focus.json)、',
f'[canonical照合](measurements/{NAME}/canonical-comparison.json)、',
f'[検証結果](measurements/{NAME}/validation.json)、',
f'[削除結果](measurements/{NAME}/previous-results-cleanup.json)。','',
'補正の再計算:','',
'```powershell',
f'python -B utils/passenger_wpr_control.py docs/measurements/passenger-wpr-generation-f9c4a85b-2026-10-18 docs/measurements/{NAME}',
'```','']
REPORT.write_text('\n'.join(lines),encoding='utf-8',newline='\n')
print(REPORT)
