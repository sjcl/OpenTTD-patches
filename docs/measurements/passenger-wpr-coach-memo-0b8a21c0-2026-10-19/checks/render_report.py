"""Render coach pre-filter, loading walk, build allocation and real-map validate results."""
from pathlib import Path
import gzip
import json
import re
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
REPORT=ROOT/'docs'/(NAME+'.md')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def ms(x):return f'{x:.6f}'
def pct(x):return '—' if x is None else f'{x:+.2f}%'
focus=load(OUT/'focus.json');canonical=load(OUT/'canonical-comparison.json');control=load(OUT/'machine-control.json')
summary=load(OUT/'summary.json');old_summary=load(BASE/'summary.json');proof=load(OUT/'validation.json')
vectors=load(OUT/'step-vector-reallocation.json');validate=load(OUT/'validate-result.json')
lines=['# 0b8a21c0: 編成内Hyperpath事前判定の再利用と構築バッファの実マップWPR','',
'`0b8a21c0f3dc9a24ca0d9d3581c19fb9c207e684` で、前回と同じ固定済み `gc3833d807.sav` からLegacyとHyperpathを各2,048 tick、WPRを1本ずつ取得した（2026-10-19 JST）。',
'比較対象は97cdc0ad。製品ソースへの追加変更なし。validateは同じ入力から別の2,048 tickとして実行し、性能比較から除外した。','']
if all(x['equal'] for x in canonical.values()):
    lines+=['**両モードとも、終了canonical 24項目すべてが97cdc0adの同じモードと完全一致した。**','']
else:
    lines+=['**終了canonicalに不一致がある。時間差を結果不変の高速化として解釈しない。**','']
for mode in ['legacy','hyperpath']:
    lines.append(f"- {mode}: {canonical[mode]['fields']}項目、{'完全一致' if canonical[mode]['equal'] else '不一致（canonical-comparison.json参照）'}。")
lines+=['','64 bit digestもPython整数として照合し、浮動小数点には変換していない。','',
'時間はCPU Usage (Sampled)のうち、メインスレッドで `StateGameLoop` を含むスタックを対象にしたinclusive CPU ms/tick。',
'同じスタック内の同じ関数は1回だけ数え、親子inclusiveを加算しない。','',
'| モード / 関数 | 97cdc0ad | 0b8a21c0 生値 | 生の変化 | 補正後 | 補正後の変化 | samples 前回→今回 |',
'| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
for mode in ['hyperpath','legacy']:
    for key,label in [('MayUseHyperpathPolicy','MayUseHyperpathPolicy'),('loading_walk','VisitBoardingCandidates<CargoLoad…>'),('StepHyperpath','StepHyperpath')]:
        if mode=='legacy' and key=='StepHyperpath':continue
        f=focus[mode][key]
        lines.append(f"| {mode} / `{label}` | {ms(f['inclusive_old_ms_per_tick'])} | {ms(f['inclusive_new_ms_per_tick'])} | {pct(f['inclusive_raw_percent'])} | {ms(f['inclusive_new_adjusted_ms_per_tick'])} | {pct(f['inclusive_adjusted_percent'])} | {f['inclusive_old_samples']:,} → {f['inclusive_new_samples']:,} |")
    if mode=='hyperpath':
        f=vectors[mode];a=f['old'];b=f['new']
        lines.append(f"| hyperpath / StepHyperpath配下のvector再確保 | {ms(a['inclusive_ms_per_tick'])} | {ms(b['inclusive_ms_per_tick'])} | {pct(f['raw_percent'])} | {ms(f['new_adjusted_ms_per_tick'])} | {pct(f['adjusted_percent'])} | {a['samples']:,} → {b['samples']:,} |")
lines+=['','loading walkの完全な関数名は `focus.json` に記録した。CargoLoad用のVisitBoardingCandidates本体だけを採り、内部lambdaのinclusiveを足していない。',
'vector再確保は、StepHyperpathを含むスタックの子に `std::vector` の `realloc` を含む関数があるサンプルを採用し、1スタックにつき1回だけ数えた。フレーム別内訳は `step-vector-reallocation.json`。',
'サンプル0は実行時間0の証明ではない。各モード1回の測定であり、測定ばらつきが含まれる。','',
'補正は `utils/passenger_wpr_control.py` の既定分類を用い、untouched時間の新/旧比を機械速度変動と仮定して新版時間を割った参考値。CPU周波数の直接測定ではない。','',
'| モード | 補正係数 新/旧 | tick実時間 ms/tick 前回→今回 | sampled CPU合計差 生値 | sampled CPU合計差 補正後 |',
'| --- | ---: | ---: | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    c=control[mode]
    lines.append(f"| {mode} | {c['machine_factor']:.9f} | {ms(old_summary[mode]['simulation_wall_ms_per_tick'])} → {ms(summary[mode]['simulation_wall_ms_per_tick'])} | {c['raw_delta_ms_per_tick']:+.6f} | {c['corrected_delta_ms_per_tick']:+.6f} |")
memo=validate['memo']
lines+=['','実マップvalidate:','','```text',f"Passenger hyperpath memo: hits={memo['hits']}; validated={memo['validated']}; violations={memo['violations']}",'```','',
f"再計算して照合したmemoは {memo['validated']:,}件、違反は {memo['violations']:,}件。",
f"validateの終了canonicalも97cdc0ad Hyperpathの24項目と{'完全一致した' if validate['canonical']['equal'] else '一致しなかった（validate-result.json参照）'}。",
'この実行は `--hyperpath-filter-mode validate`、WPRなし。通常のindexモードのWPR結果と混ぜていない。','',
'計測条件・検証:','',
'- 同じセーブ・設定・245アセットをSHA-256で確認。StateTicks 3,197,276,448 → 3,197,278,496。実tickサンプル各2,048。',
'- 最初は前回と同じLegacy→Hyperpathの順。LegacyのMemory記録がマーカー監査に通らなかったため、Hyperpathの後にLegacyだけFile方式で取り直した。`--resume --precomputed --warm-precomputed --wait-background-ready`、Hyperpath索引ON、メインスレッドaffinity mask=1。',
'- 今回のロード手順ではgame_start時に保存済みテーブルの復元が未完了だったため、`--wait-saved-replay` を開始前に指定した。復元完了後にモードを設定し、停止中のwarm-up・背景処理待ちを行った。準備でtickが進んでいないことを確認。追加の定常化・cold・全再構築なし。',
'- MSVC 19.44 x64 Release、`/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert ON、LTO OFF、静的CRT。前回と同じコンパイル・リンク設定。',
'- **採取バッファを拡大したため、両モードの時間比較に記録構成の差が含まれる。** Hyperpathの保存方式は前回・今回ともFile。system buffers 20→1,024、event buffers 20→128。Legacyは前回Memory→今回File、system buffers 4,781→1,024、event buffers 20→128。両モードで1秒の停止中準備とreadyマーカーを置いてからbeginを記録した。',
'- CPU採取のprovider・keywords・stacksは前回のプロファイルを保持し、変更は上記のバッファ数2項目だけであることをXMLで検証した。サンプリング間隔は1ms。WPAプロファイルとツールも同じ。',
f"- 全{proof['native_tests']:,} nativeテスト・{proof['native_assertions']:,} assertionsが通った。",
'- 両区間で開始・終了マーカーを確認。kernel監査で競合するビルド・テスト・ゲームなし。lost events / buffersは0、製品関数の未解決シンボルなし。',
'- 生CSVのSHA-256、スタックとexclusiveの時間・サンプル数の保存則、支払い整合性を検証。WPAのInvalid Event警告は測定マーカー外であることを確認。',
'- 新しい準備コマンドでタイマーが2か所になったため、計測用ヘルパーのコピー側だけでwarm-up挿入位置を限定した。挿入する処理は前回と同じで、差分とハッシュはprepare-compat.jsonに保存した。','',
'最初のLegacy試行は、復元が未完了でwarm-upと背景待ちが失敗したため中断・不採用とした。この記録は性能集計に含めていない。メタデータとログをchecks/rejected-incomplete-save-replay/に保存し、大きな不採用ETLは削除した。','',
'復元後のLegacy Memory実行ではcanonical 24項目は一致したが、ETLに開始マーカーがなく時間比較から除外した。checks/rejected-legacy-memory-markers/に薄い証跡を保存し、大きなETLは削除した。性能集計には、開始・終了マーカーの揃ったFile再実行だけを採用した。','',
'最初のHyperpath File実行もcanonicalは完全一致したが、WPRが2,024,287件のイベント欠落を報告したため不採用とした。バッファを拡大して取り直し、lost events / buffersが0の記録だけを採用した。不採用分の薄い証跡はchecks/rejected-hyperpath-dropped-events/に保存した。','',
'前回同様、JP+ Multiple UnitsのNewGRF警告がある入力を使用し、警告を抑制する変更はしていない。','']
for mode in ['legacy','hyperpath']:
    now=(WORK/mode/'stderr.txt').read_text(encoding='utf-8-sig',errors='replace')
    before=gzip.decompress((BASE/mode/'stderr.txt.gz').read_bytes()).decode('utf-8-sig',errors='replace')
    count=lambda t:len(re.findall(r"Behaviour of NewGRF .* is likely to cause desyncs and/or crashes",t))
    resumes=(WORK/mode/'on-1-console.txt').read_text(encoding='utf-8-sig').count('Measurement resumed a NewGRF error pause.')
    lines.append(f'- {mode}: NewGRF警告 {count(before):,} → {count(now):,}件、今回の計測フックによるerror pause解除 {resumes}回。')
lines+=['','ETL・対応exe/PDBは作業ツリー外にも保存し、場所とSHA-256をrecording-backup.jsonに記録した。固定入力は既存の同一ZIPを参照し、重複ZIPは作っていない。','',
f'[関数別inclusive/exclusive差分CSV](measurements/{NAME}/function-before-after.csv)、',
f'[指定関数の全数値](measurements/{NAME}/focus.json)、',
f'[vector再確保の内訳](measurements/{NAME}/step-vector-reallocation.json)、',
f'[canonical照合](measurements/{NAME}/canonical-comparison.json)、',
f'[validate結果](measurements/{NAME}/validate-result.json)、',
f'[検証結果](measurements/{NAME}/validation.json)。','',
'補正の再計算:','','```powershell',
f'python -B utils/passenger_wpr_control.py docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19 docs/measurements/{NAME}',
'```','']
REPORT.write_text('\n'.join(lines),encoding='utf-8',newline='\n')
print(REPORT,flush=True)
