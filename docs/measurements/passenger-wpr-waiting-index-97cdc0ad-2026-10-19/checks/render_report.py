"""Render all six requested inclusive comparisons without adding parent and child time."""
from pathlib import Path
import gzip
import json
import re
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18'
REPORT=ROOT/'docs'/(NAME+'.md')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def ms(x):return f'{x:.6f}'
def pct(x):return '—' if x is None else f'{x:+.2f}%'
focus=load(OUT/'focus.json');canonical=load(OUT/'canonical-comparison.json');control=load(OUT/'machine-control.json')
summary=load(OUT/'summary.json');old_summary=load(BASE/'summary.json');proof=load(OUT/'validation.json')
names=['RoutePassengerJourneys','RecordCapacityMiss','ForgetWaitingPacket','IndexPacket','AddBoardingEntry','RemoveBoardingEntry']
lines=['# 97cdc0ad: 待機パケット索引更新の実マップWPR','',
'`97cdc0ad4c9493daea0db6079e90a03924d96b56` で、前回と同じ固定済み `gc3833d807.sav` からLegacyとHyperpathを各2,048 tick、1本ずつ計測した（2026-10-19 JST）。',
'比較対象は9ab0228d。製品ソースへの追加変更はしていない。','']
if all(x['equal'] for x in canonical.values()):
    lines+=['**両モードとも、終了canonical 24項目すべてが9ab0228dの同じモードと完全一致した。**','']
else:
    lines+=['**終了canonicalに不一致がある。時間差を結果不変の高速化として解釈しない。**','']
for mode in ['legacy','hyperpath']:
    lines.append(f"- {mode}: {canonical[mode]['fields']}項目、{'完全一致' if canonical[mode]['equal'] else '不一致（canonical-comparison.json参照）'}。")
lines+=['','64 bit digestもPython整数として比較し、浮動小数点には変換していない。','',
'時間はCPU Usage (Sampled)のうち、メインスレッドで `StateGameLoop` を含むスタックを対象にしたinclusive CPU ms/tick。',
'同じスタック内の同じ関数は1回だけ数え、親子関数のinclusiveを加算しない。6関数はいずれも `StationCargoList` のメソッド。','',
'| モード / 関数 | 9ab0228d | 97cdc0ad 生値 | 生の変化 | 補正後 | 補正後の変化 | samples 前回→今回 |',
'| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
for mode in ['hyperpath','legacy']:
    for name in names:
        f=focus[mode]['openttd.exe!StationCargoList::'+name]
        lines.append(f"| {mode} / `{name}` | {ms(f['inclusive_old_ms_per_tick'])} | {ms(f['inclusive_new_ms_per_tick'])} | {pct(f['inclusive_raw_percent'])} | {ms(f['inclusive_new_adjusted_ms_per_tick'])} | {pct(f['inclusive_adjusted_percent'])} | {f['inclusive_old_samples']:,} → {f['inclusive_new_samples']:,} |")
lines+=['','サンプル0は実行時間0の証明ではない。インライン展開や短い処理では採取されない場合がある。',
'補正は `utils/passenger_wpr_control.py` の既定分類による。untouched時間の新/旧比を機械速度変動と仮定し、新版時間をその比で割った参考値で、CPU周波数の直接測定ではない。',
'各モード1回の測定であり、測定ばらつきが含まれる。','',
'| モード | 補正係数 新/旧 | tick全体 実時間 ms/tick 前回→今回 |',
'| --- | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    lines.append(f"| {mode} | {control[mode]['machine_factor']:.9f} | {ms(old_summary[mode]['simulation_wall_ms_per_tick'])} → {ms(summary[mode]['simulation_wall_ms_per_tick'])} |")
lines+=['','StateGameLoopを含むスタックのsampled CPU合計差（親子inclusiveを加算した値ではない）:','',
'| モード | 生の差 ms/tick | 補正後の差 ms/tick |',
'| --- | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    lines.append(f"| {mode} | {control[mode]['raw_delta_ms_per_tick']:+.6f} | {control[mode]['corrected_delta_ms_per_tick']:+.6f} |")
lines+=['','計測条件と検証:','',
'- 同じセーブ・設定・245アセットをSHA-256で確認。StateTicks 3,197,276,448 → 3,197,278,496、実tickサンプル各2,048。',
'- 前回と同じLegacy→Hyperpathの順。`--resume --precomputed --warm-precomputed --wait-background-ready`、Hyperpath索引ON、メインスレッドaffinity mask=1。',
'- 初期の準備中は停止し、背景処理完了までtickが進んでいないことを確認。追加の定常化・cold・全再構築なし。',
'- MSVC 19.44 x64 Release、`/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert ON、LTO OFF、静的CRT。前回と同じコンパイル・リンク設定。',
'- **WPR保存方式も同じモード同士で一致**。Legacyは前回・今回ともMemory、Hyperpathは前回・今回ともFile。Hyperpathは同じ1秒の停止中準備とreadyマーカーを入れてからbeginマーカーを記録した。',
'- CPUプロファイルのprovider・keywords・stacks・buffersを含む設定を、XML要素の出力順を除いて照合し、一致を確認した。WPAプロファイルとツールも同じ。',
f"- 全{proof['native_tests']:,} nativeテスト・{proof['native_assertions']:,} assertionsが通った。",
'- 両記録の開始・終了マーカーを確認し、kernelプロセス監査で競合ビルド・テスト・ゲームなし。lost events / buffersは0、製品関数の未解決シンボルなし。',
'- 生CSVのSHA-256、スタックとexclusiveの時間・サンプル数の保存則、支払い整合性を検証。WPAのInvalid Event警告がある場合、その時刻が測定マーカー外にあることも確認した。','',
'前回と同じJP+ Multiple UnitsのNewGRF警告が出る入力。警告を抑制する変更はしていない。','']
for mode in ['legacy','hyperpath']:
    now=(WORK/mode/'stderr.txt').read_text(encoding='utf-8-sig',errors='replace')
    before=gzip.decompress((BASE/mode/'stderr.txt.gz').read_bytes()).decode('utf-8-sig',errors='replace')
    count=lambda t:len(re.findall(r"Behaviour of NewGRF .* is likely to cause desyncs and/or crashes",t))
    resumes=(WORK/mode/'on-1-console.txt').read_text(encoding='utf-8-sig').count('Measurement resumed a NewGRF error pause.')
    lines.append(f'- {mode}: NewGRF警告 {count(before):,} → {count(now):,}件、今回の計測フックによるerror pause解除 {resumes}回。')
lines+=['','ETLと対応exe/PDBは作業ツリー外にも保持し、場所・SHA-256を `recording-backup.json` に記録した。',
'固定入力は9ab0228dと同じ既存の検証済みZIPを参照し、重複ZIPは作っていない。','',
f'[全関数のinclusive/exclusive差分CSV](measurements/{NAME}/function-before-after.csv)、',
f'[6関数の全数値](measurements/{NAME}/focus.json)、',
f'[canonical照合](measurements/{NAME}/canonical-comparison.json)、',
f'[検証結果](measurements/{NAME}/validation.json)。','',
'補正の再計算:','','```powershell',
f'python -B utils/passenger_wpr_control.py docs/measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18 docs/measurements/{NAME}',
'```','']
REPORT.write_text('\n'.join(lines),encoding='utf-8',newline='\n')
print(REPORT,flush=True)
