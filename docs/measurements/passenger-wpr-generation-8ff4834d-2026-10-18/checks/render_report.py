"""Render the two ordinary 8ff4834d captures and their requested comparison."""
from pathlib import Path
import json

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-generation-8ff4834d-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
REPORT=ROOT/'docs'/(NAME+'.md')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
c=load(OUT/'comparison.json');s=load(OUT/'summary.json');v=load(OUT/'validation.json')
control=load(OUT/'machine-control.json')
def n(x):return f'{x:,}'
def ms(x):return f'{x:.6f}'
def pct(x):return f'{x:+.2f}%'
lines=['# 8ff4834d: 通常条件のLegacy / Hyperpath WPR','',
'`8ff4834dba34f6ae18ea2068a542ecf47a4ac5e5` で、各モードの定常入力から通常条件を各2,048 tick、1回ずつ記録した。',
'比較対象は前回の88cb61e3通常条件（candidate）。終了canonicalは保存したが、一致判定には使っていない。','',
'**Legacyの期限超過の滞留は解消していない（overdue=87,035、oldest_ticks=588）。**',
'generation処理回数はLegacyがほぼ不変、Hyperpathが2.55%減だった。','',
'`cause=generation` の packets は処理回数で、同じパケットの再処理を含む。時間は診断カウンターの実時間の合計。',
'WPRのinclusive CPU時間とは計測方法が異なる。各ms/tickは2,048で割った値。','',
'| モード | generation処理回数 前回 → 今回 | 診断時間合計 ms 前回 → 今回 | 診断 ms/tick 前回 → 今回 | 処理回数 / 時間の変化 |',
'| --- | ---: | ---: | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    r=c[mode];a=r['previous']['generation'];b=r['current']['generation']
    lines.append(f"| {mode} | {n(a['packets'])} → {n(b['packets'])} | {ms(a['microseconds']/1000)} → {ms(b['microseconds']/1000)} | {ms(a['diagnostic_wall_ms_per_tick'])} → {ms(b['diagnostic_wall_ms_per_tick'])} | {pct(r['generation_packet_percent'])} / {pct(r['generation_diagnostic_time_percent'])} |")
lines+=['','`RunPassengerWaitingEvents` はCPU Usage (Sampled)の `StateGameLoop` を含むスタックが対象。',
'各スタック内で同じ関数を1回だけ数えたinclusiveで、親子のinclusiveは加算しない。','',
'| モード | 前回 CPU ms/tick | 今回 CPU ms/tick | 変化 | 今回inclusive samples |',
'| --- | ---: | ---: | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    r=c[mode];a=r['previous']['RunPassengerWaitingEvents'];b=r['current']['RunPassengerWaitingEvents']
    lines.append(f"| {mode} | {ms(a['inclusive_cpu_ms_per_tick'])} | {ms(b['inclusive_cpu_ms_per_tick'])} | {pct(r['waiting_inclusive_cpu_percent'])} | {n(b['inclusive_samples'])} |")
lines+=['','終了時の処理待ち:','',
'| モード | pending 前回 → 今回 | overdue 前回 → 今回 | oldest_ticks 前回 → 今回 | maximum_station_backlog 前回 → 今回 |',
'| --- | ---: | ---: | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    r=c[mode];a=r['previous']['events'];b=r['current']['events']
    lines.append('| '+mode+' | '+' | '.join(f'{n(a[k])} → {n(b[k])}' for k in ['pending','overdue','oldest_ticks','maximum_station_backlog'])+' |')
b=c['legacy']['current']['events']
if c['legacy']['overdue_cleared_at_end']:
    lines+=['','**Legacyは終了時に overdue=0、oldest_ticks=0となり、この区間の終了時点では期限超過の滞留が解消した。**']
else:
    lines+=['',f"**Legacyは終了時にも overdue={n(b['overdue'])}、oldest_ticks={n(b['oldest_ticks'])} が残り、期限超過の滞留は解消していない。**"]
lines+=['pendingには将来のイベントも含まれる。上の表は終了時点の値であり、区間内の最大overdueは測っていない。','',
'tick全体の実時間（計測対象のシミュレーションのみ）:','',
'| モード | 前回 ms/tick | 今回 ms/tick | 変化 |',
'| --- | ---: | ---: | ---: |']
old=load(ROOT/'docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/candidate/summary.json')
for mode in ['legacy','hyperpath']:
    a=old[mode]['simulation_wall_ms_per_tick'];b=s[mode]['simulation_wall_ms_per_tick']
    lines.append(f'| {mode} | {ms(a)} | {ms(b)} | {pct(100*(b/a-1))} |')
lines+=['','各モード1回のサンプリング測定で、小さい差には測定ばらつきが含まれる。',
'前回からjgrpp-ya統合、キューの差し戻し、非同期構築なども入っているため、差のすべてを8ff4834dの単独変更へ帰属させない。',
'`gui.passenger_precomputed_workers` は今回の既定4。構築を強制せず、停止中のバックグラウンド準備完了を待ってから測った。','',
'指定の `passenger_wpr_control.py` による補正も参考値として保存した。',
'処理件数と終了状態が変わる比較なので、同じ仕事量・共通の速度変動を仮定する補正を主な結論には使わない。',
f"係数（新/旧）はLegacy {control['legacy']['machine_factor']:.9f}、Hyperpath {control['hyperpath']['machine_factor']:.9f}。",'',
f"参考補正後の `RunPassengerWaitingEvents` inclusiveはLegacy {c['legacy']['current']['RunPassengerWaitingEvents']['inclusive_cpu_ms_per_tick']/control['legacy']['machine_factor']:.6f}、Hyperpath {c['hyperpath']['current']['RunPassengerWaitingEvents']['inclusive_cpu_ms_per_tick']/control['hyperpath']['machine_factor']:.6f} CPU ms/tick。",'',
'測定条件と記録の検証:','',
'- StateTicks 13,647,872 → 13,649,920。各2,048個の実tickサンプル。`--resume --warm-precomputed --wait-background-ready`。',
'- Hyperpath索引ON (`--hyperpath-filter-mode index`)。メインスレッドaffinity mask=1。再構築・cold条件・追加のトレースは使っていない。',
'- MSVC 19.44 x64 Release、`/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert ON、LTO OFF、同じCPU WPRプロファイルとWPAエクスポーター。',
'- 全763 nativeテスト、10,586,640 assertionsが通った。製品ソースへの追加変更はない。',
'- 両区間のkernelプロセス監査に競合ビルド・テスト・ゲームなし。lost events / buffersは0、製品関数の未解決シンボルなし。',
'- 生CSVのSHA-256、サンプル数とexclusive時間の保存則、支払い整合性を検証。WPA警告の時刻は有無を含め `validation.json` に保存し、すべて計測マーカー外であることを確認した。','',
'入力の互換性:','',
'前回の8462e6cd定常セーブは未公開の開発版形式（feature 3/3/17、JPSD 15）だった。',
'jgrpp-ya統合後の8ff4834dは整理後の形式1を受け付けるため、元入力のロードは拒否された。この試行はWPR開始前に失敗しており、結果に含めていない。',
'元セーブを保存したまま、コピーのfeature番号を整理し、製品側で固定された旧アルゴリズム設定など15 words（120 bytes）をJPSDから取り除いた。',
'シミュレーションを進めずに変換し、8ff4834dで0 tickロード・再保存検証を各モードで実施した。',
'初期canonicalの実状態22項目が元の定常入力と一致した。変換したJPSDと0 tickで再保存したJPSDは、全体がバイト単位で一致した。',
'形式に依存する `runtime_routing_feature` と `database_digest` の2項目は、この初期入力の確認から除いた。',
'現在のスイープ管理項目は旧形式ロード時の製品既定で初期化される。終了時canonicalの比較はしていない。','',
'変換内容・旧新SHA-256・0 tick検証は `input-conversion.json` と `input-conversion-validation.json`、変換コードは `checks/convert_inputs.py`。',
'元入力と共有アセットのZIP、および変換済み入力ZIPを作業ツリー外に保持した。ETLと対応exe/PDBも作業ツリー外で保持し、ハッシュを確認した。',
'保存場所とSHA-256は `input-backup.json`、`converted-input-backup.json`、`recording-backup.json` に記録している。','',
f'[比較の全数値](measurements/{NAME}/comparison.json)、',
f'[関数別inclusive/exclusive差分CSV](measurements/{NAME}/function-before-after.csv)、',
f'[Legacy関数CSV](measurements/{NAME}/legacy-functions.csv)、',
f'[Hyperpath関数CSV](measurements/{NAME}/hyperpath-functions.csv)、',
f'[WPR・入力・生CSVの検証](measurements/{NAME}/validation.json)。','',
'機械補正の再計算:','',
'```powershell',
f'python -B utils/passenger_wpr_control.py docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/candidate docs/measurements/{NAME}',
'```','']
REPORT.write_text('\n'.join(lines),encoding='utf-8',newline='\n')
print(REPORT)
