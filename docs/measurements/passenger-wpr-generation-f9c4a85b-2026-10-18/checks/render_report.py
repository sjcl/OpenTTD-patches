"""Render the requested new-input generation sweep and overdue measurements."""
from pathlib import Path
import json
import re

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-generation-f9c4a85b-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
REPORT=ROOT/'docs'/(NAME+'.md')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
m=load(OUT/'metrics.json');s=load(OUT/'summary.json');v=load(OUT/'validation.json');inputs=load(OUT/'input-validation.json')
def n(x):return f'{x:,}'
def ms(x):return f'{x:.6f}'
legacy=m['runs']['legacy'];end=legacy['current']['events']
lines=['# f9c4a85b: gc3833d807.savでの通常条件WPR','',
'`f9c4a85b721073bbf86747f4a4cec7ac5ccd2f89` で、指定された `gc3833d807.sav` からLegacyとHyperpathを各2,048 tick、1回ずつ記録した。',
'両モードとも同じ元セーブのコピーを直接使った。形式変換や8,192 tickの定常化は行っていない。',
'停止中にモードを設定し、表とロード済みバックグラウンド処理の準備完了を待った。0 tick検証の結果セーブは計測入力に使っていない。','']
if legacy['overdue_cleared_at_end']:
    lines+=['**Legacyは終了時に overdue=0、oldest_ticks=0となり、この終了時点の期限超過の滞留は解消していた。**','']
elif end['oldest_ticks']==0:
    lines+=[f"**Legacyは終了時に overdue={n(end['overdue'])}、oldest_ticks=0。overdueは残るが、1 tick以上の遅延は表示されていない。**",'']
else:
    lines+=[f"**Legacyは終了時にも overdue={n(end['overdue'])}、oldest_ticks={n(end['oldest_ticks'])} が残った。期限超過の滞留は解消していない。**",'']
lines+=['`cause=generation` の packets は同じパケットの再処理を含む処理回数。診断時間は該当処理の実時間の合計で、WPRのCPU時間とは別の測定値。',
'ms/tickは各合計を2,048で割った。','',
'| モード | generation処理回数 | 診断時間合計 ms | 診断 ms/tick | unchanged generation件数 |',
'| --- | ---: | ---: | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    c=m['runs'][mode]['current'];g=c['generation']
    lines.append(f"| {mode} | {n(g['packets'])} | {ms(g['microseconds']/1000)} | {ms(g['diagnostic_wall_ms_per_tick'])} | {n(c['unchanged_generation_packets'])} |")
lines+=['','`RunPassengerWaitingEvents` はCPU Usage (Sampled)の `StateGameLoop` を含むスタックが対象。',
'各スタック内で同じ関数を1回だけ数え、親子のinclusiveは加算していない。','',
'| モード | inclusive CPU ms/tick | exclusive CPU ms/tick | inclusive samples | tick全体 実時間 ms/tick |',
'| --- | ---: | ---: | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    w=m['runs'][mode]['current']['RunPassengerWaitingEvents']
    lines.append(f"| {mode} | {ms(w['inclusive_ms_per_tick'])} | {ms(w['exclusive_ms_per_tick'])} | {n(w['inclusive_samples'])} | {ms(s[mode]['simulation_wall_ms_per_tick'])} |")
lines+=['','終了時の処理待ち:','',
'| モード | pending | overdue | oldest_ticks | maximum_station_backlog |',
'| --- | ---: | ---: | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    e=m['runs'][mode]['current']['events']
    lines.append('| '+mode+' | '+' | '.join(n(e[k]) for k in ['pending','overdue','oldest_ticks','maximum_station_backlog'])+' |')
lines+=['','pendingには将来のイベントも含む。overdueは現在tickちょうどが期限の予定も数えるため、oldest_ticksもあわせて読む。終了時点の値であり、区間内の最大overdueや解消時刻は測っていない。',
'0 tickの別ロードで観測した初期イベント状態は `metrics.json` の `initial_zero_tick_events` に残した。',
'**スイープの時期が変わるため、終了canonicalは比較に使用していない。**','',
'前回8ff4834dの値との対照（参考）:','',
'| モード | 前回generation件数 → 今回 | 前回診断 ms/tick → 今回 | 前回waiting inclusive CPU ms/tick → 今回 |',
'| --- | ---: | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    a=m['runs'][mode]['previous_8ff4834d_reference'];b=m['runs'][mode]['current']
    lines.append(f"| {mode} | {n(a['generation']['packets'])} → {n(b['generation']['packets'])} | {ms(a['generation']['diagnostic_wall_ms_per_tick'])} → {ms(b['generation']['diagnostic_wall_ms_per_tick'])} | {ms(a['RunPassengerWaitingEvents']['inclusive_cpu_ms_per_tick'])} → {ms(b['RunPassengerWaitingEvents']['inclusive_ms_per_tick'])} |")
lines+=['','**前回とは入力セーブが異なる。** この対照を同じ仕事量での改善率や、f9c4a85bだけの効果とは解釈しない。',
'今回の入力のSHA-256は両モードで同一。前回はモードごとに別の定常セーブだった。機械速度補正は行っていない。',
'各モード1回のサンプリング測定であり、小さい時間差には測定ばらつきが含まれる。','',
'計測と検証:','',
f"- StateTicks {n(inputs['start_tick'])} → {n(inputs['target_end_tick'])}。各2,048個の実tickサンプル。停止中の準備ではStateTicksが進まないことを確認した。",
'- `--resume --precomputed --warm-precomputed --wait-background-ready`、Hyperpath索引ON (`--hyperpath-filter-mode index`)。メインスレッドaffinity mask=1。再構築・cold条件・追加のトレースなし。',
'- MSVC 19.44 x64 Release、`/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert ON、LTO OFF。前回と同じCPU WPRプロファイルとWPAエクスポーター。',
'- 全763 nativeテスト・10,586,649 assertionsが通った。製品ソースへの追加変更なし。計測のためにコピーしたTUのみを使った。',
'- 両記録区間のkernelプロセス監査で競合ビルド・テスト・ゲームなし。lost events / buffersは0、製品関数の未解決シンボルなし。',
'- 生CSVのSHA-256、サンプル数とexclusive時間の保存則、支払い整合性を検証。WPA Invalid Event警告はすべて計測マーカー外。','',
'WPA書き出しの初回はサンドボックスのレジストリ読み取り制限で終了した。ETLを変更せず、通常の実行環境でCSVを書き出し直した。成功したCSVのみを集計し、初回のログは `checks/failed-export/` に保存した。','',
'JP+ Multiple Unitsの車両容量変更に関するNewGRF警告が出た。0 tick検証より2,048 tick実行のほうが警告数が多く、計測値は警告が発生するこの状態での値である。stderr全体を保存し、セーブやNewGRFの修正はしていない。','']
for mode in ['legacy','hyperpath']:
    text=(WORK/mode/'stderr.txt').read_text(encoding='utf-8-sig',errors='replace')
    count=len(re.findall(r"Behaviour of NewGRF .* is likely to cause desyncs and/or crashes",text))
    zero=(WORK/('validation-'+mode)/'stderr.txt').read_text(encoding='utf-8-sig',errors='replace')
    zero_count=len(re.findall(r"Behaviour of NewGRF .* is likely to cause desyncs and/or crashes",zero))
    resumes=(WORK/mode/'on-1-console.txt').read_text(encoding='utf-8-sig').count('Measurement resumed a NewGRF error pause.')
    lines.append(f'- {mode}: 0 tick検証 {n(zero_count)}件、2,048 tick実行 {n(count)}件。計測用フックによるNewGRF error pause再開 {n(resumes)}回。')
lines+=['','元ファイルからのコピー・SHA-256と0 tickロード検証は `input-validation.json`。入力と全アセットのZIP、ETL、対応exe/PDBを作業ツリー外に保持し、ハッシュを検証した。',
'保存場所とSHA-256は `input-backup.json` と `recording-backup.json`。終了セーブのcanonicalは証跡として残したが、等価性判定には使っていない。','',
f'[計測値](measurements/{NAME}/metrics.json)、',
f'[Legacy関数別inclusive/exclusive CSV](measurements/{NAME}/legacy-functions.csv)、',
f'[Hyperpath関数別inclusive/exclusive CSV](measurements/{NAME}/hyperpath-functions.csv)、',
f'[検証結果](measurements/{NAME}/validation.json)。','']
REPORT.write_text('\n'.join(lines),encoding='utf-8',newline='\n')
print(REPORT)
