"""Report matched kernel-CPU controls, historical references, and memo validation."""
from pathlib import Path
import json
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
REPORT=ROOT/'docs'/(NAME+'.md')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def ms(x):return f'{x:.6f}'
def pct(x):return '—' if x is None else f'{x:+.2f}%'
focus=load(OUT/'focus.json');v=load(OUT/'validation.json');control=load(OUT/'machine-control.json')
vectors=load(OUT/'step-vector-reallocation.json');historical=load(OUT/'historical-focus.json')
summary=load(OUT/'summary.json');old=load(OUT/'control-97cdc0ad/summary.json');validate=load(OUT/'validate-result.json')
lines=['# 0b8a21c0: 編成内Hyperpath判定再利用の実マップ計測','',
'同じ固定済み `gc3833d807.sav` で、`0b8a21c0f3dc9a24ca0d9d3581c19fb9c207e684` と `97cdc0ad4c9493daea0db6079e90a03924d96b56` を各Legacy・Hyperpath、2,048 tickずつ記録した（2026-10-19 JST）。',
'ETWの欠落対策で採取プロファイルを変更したため、**97cdc0adも同じ新しいプロファイルで追加測定し、同条件同士を主比較にした**。各組合せ1回、採用WPRは計4本。製品ソースへの追加変更なし。','']
if validate['memo']['violations']:
    lines+= [f"**validateは不合格: memoのviolations={validate['memo']['violations']:,}（validated={validate['memo']['validated']:,}）。0ではなく、この実マップで再利用の正しさは確認できなかった。**",
        '終了canonicalの一致は、このmemo検証の不一致を打ち消すものではない。','']
all_equal=all(r['canonical_equal'] for group in [v['runs'],v['control_runs']] for r in group.values())
lines+=['**新版両モード・追加基準両モードとも、終了canonical 24項目が以前の97cdc0adの同じモードと完全一致した。**' if all_equal else '**終了canonicalに不一致がある。validation.jsonの差分を参照。**','',
'64 bit digestを含めPython整数で比較し、浮動小数点には変換していない。','',
'時間はCPU Usage (Sampled)のうち、メインスレッドで `StateGameLoop` を含むスタックのinclusive CPU ms/tick。スタック内の同じ関数は1回だけ数え、親子inclusiveは加算しない。','',
'| モード / 関数 | 同条件97cdc0ad | 0b8a21c0 生値 | 生の変化 | 0b8a21c0 補正後 | 補正後の変化 | samples 旧→新 |',
'| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
for mode in ['hyperpath','legacy']:
    for key,label in [('MayUseHyperpathPolicy','MayUseHyperpathPolicy'),('loading_walk','VisitBoardingCandidates<CargoLoad…>'),('StepHyperpath','StepHyperpath')]:
        if mode=='legacy' and key=='StepHyperpath':continue
        f=focus[mode][key]
        lines.append(f"| {mode} / `{label}` | {ms(f['inclusive_old_ms_per_tick'])} | {ms(f['inclusive_new_ms_per_tick'])} | {pct(f['inclusive_raw_percent'])} | {ms(f['inclusive_new_adjusted_ms_per_tick'])} | {pct(f['inclusive_adjusted_percent'])} | {f['inclusive_old_samples']:,} → {f['inclusive_new_samples']:,} |")
    if mode=='hyperpath':
        f=vectors[mode];a=f['old'];b=f['new']
        lines.append(f"| hyperpath / StepHyperpath配下のvector再確保 | {ms(a['inclusive_ms_per_tick'])} | {ms(b['inclusive_ms_per_tick'])} | {pct(f['raw_percent'])} | {ms(f['new_adjusted_ms_per_tick'])} | {pct(f['adjusted_percent'])} | {a['samples']:,} → {b['samples']:,} |")
        dominant=max(a['frames'],key=lambda name:a['frames'][name]['inclusive_ms_per_tick'])
        da=a['frames'][dominant];db=b['frames'].get(dominant,dict(inclusive_ms_per_tick=0,samples=0))
        x=da['inclusive_ms_per_tick'];y=db['inclusive_ms_per_tick'];factor=control[mode]['machine_factor']
        lines.append(f"| hyperpath / 再確保の最大成分 | {ms(x)} | {ms(y)} | {pct(100*(y/x-1))} | {ms(y/factor)} | {pct(100*(y/factor/x-1))} | {da['samples']:,} → {db['samples']:,} |")
lines+=['','loading walkはCargoLoad用VisitBoardingCandidates本体だけで、内部lambdaを足していない。完全なシンボル名はfocus.json。',
'vector再確保はStepHyperpathの子に `std::vector` の `realloc` を含むフレームがあるスタックを1回だけ数えた。内訳はstep-vector-reallocation.json。サンプル0は実行時間0の証明ではない。','',
'約0.22 ms/tickだった最大成分は、PDB上で `std::vector<LinkGraph::EdgeRoute>::_Emplace_reallocate` と表示される成分。上表の「最大成分」は同じシンボルを新旧で照合した値で、再確保合計の内数。','',
'過去の通常CPUプロファイルによる97cdc0adの参考値（今回の主比較の基準値とは採取条件が異なる）:','',
'| Hyperpath関数 | 前回97cdc0ad | 今回同条件97cdc0ad | 今回0b8a21c0 |','| --- | ---: | ---: | ---: |']
for key,label in [('MayUseHyperpathPolicy','MayUseHyperpathPolicy'),('loading_walk','loading walk'),('StepHyperpath','StepHyperpath')]:
    lines.append(f"| {label} | {ms(historical['hyperpath'][key]['inclusive_ms_per_tick'])} | {ms(focus['hyperpath'][key]['inclusive_old_ms_per_tick'])} | {ms(focus['hyperpath'][key]['inclusive_new_ms_per_tick'])} |")
lines.append(f"| StepHyperpath配下vector再確保 | {ms(historical['hyperpath']['vector_reallocation']['inclusive_ms_per_tick'])} | {ms(vectors['hyperpath']['old']['inclusive_ms_per_tick'])} | {ms(vectors['hyperpath']['new']['inclusive_ms_per_tick'])} |")
lines+=['','補正は `utils/passenger_wpr_control.py` の既定分類。untouched時間の新/旧比を機械速度変動と仮定し、新版時間を割った参考値で、CPU周波数の直接測定ではない。測定ばらつきを含む。','',
'| モード | 補正係数 新/旧 | tick実時間 ms/tick 同条件97→0b | sampled CPU合計差 生値 | 補正後 |','| --- | ---: | ---: | ---: | ---: |']
for mode in ['legacy','hyperpath']:
    c=control[mode]
    lines.append(f"| {mode} | {c['machine_factor']:.9f} | {ms(old[mode]['simulation_wall_ms_per_tick'])} → {ms(summary[mode]['simulation_wall_ms_per_tick'])} | {c['raw_delta_ms_per_tick']:+.6f} | {c['corrected_delta_ms_per_tick']:+.6f} |")
memo=validate['memo']
lines+=['','実マップvalidate:','','```text',f"Passenger hyperpath memo: hits={memo['hits']}; validated={memo['validated']}; violations={memo['violations']}",'```','',
f"再計算照合 {memo['validated']:,}件、違反 {memo['violations']:,}件。validate終了canonicalも以前の97cdc0ad Hyperpathと{'24項目完全一致' if validate['canonical']['equal'] else '不一致（validate-result.json参照）'}。",
'このviolationsカウンターには、保存済みmemo値と再計算値の不一致、およびuniqueとしたグループの重複検出の2か所が加算する。今回の集計値だけではどちらが原因かを分離できない（cargopacket.cppのhyperpath_memo_violations加算箇所）。',
'同じ入力から別の2,048 tickとして `--hyperpath-filter-mode validate` で実行した。WPRなしで、validateの時間を性能比較に使っていない。','',
'条件と検証:','',
'- 同じセーブ・設定・245アセットをSHA-256で確認。StateTicks 3,197,276,448 → 3,197,278,496、実tickサンプル各2,048。',
'- 新旧とも `--resume --precomputed --warm-precomputed --wait-background-ready`、Hyperpath索引ON、メインスレッドaffinity mask=1。旧版Legacy→旧版Hyperpath→新版Legacy→新版Hyperpathの順。',
'- 新版はロード手順変更に合わせ、停止中に `--wait-saved-replay` で復元完了を待ってから設定・warm-upを行った。全4本でwarm-up成功・背景処理完了・準備前後のtick不変を確認。追加の定常化・cold・全再構築なし。',
'- 新旧ともMSVC 19.44 x64 Release、`/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert ON、LTO OFF、静的CRT。旧版は以前保存したexe/PDBのSHA-256と一致を確認して使用。',
f"- 新版は全{v['native_tests']:,} nativeテスト・{v['native_assertions']:,} assertionsが通った。",
'- 全4本で同一の `cpu-minimal.wprp`。File方式、1ms間隔、system buffersは1,024×1,024KiB。kernelのCpuConfig・Loader・ProcessThread・SampledProfileとSampledProfileスタックだけを採取した。WPAの表示・集計プロファイルも同じ。',
'- 全4本でlost events / buffersが0、開始・終了マーカーあり。kernelプロセス監査で競合ビルド・テスト・ゲームなし。製品関数の未解決シンボルなし。',
'- 生CSVのSHA-256、スタックとexclusiveの時間・サンプル数の保存則、支払い整合性を検証。WPA警告は測定マーカー外であることを確認した。','',
'記録方法を変更した理由:','',
'通常CPUプロファイルでは、復元未完了の試行、Legacyの開始マーカー欠落、File方式のイベント欠落が生じた。system/eventバッファ拡大とCSwitch等の除外でも欠落が残ったため、ユーザーイベントコレクターを含めて除いたkernel CPU専用プロファイルへ変更した。',
'短い試験で欠落0と両マーカーを確認した後、実マップを新旧同一条件で取り直した。欠落ありの記録を時間比較には使っていない。[MicrosoftのETWセッション説明](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/sessions)と[Xperfの欠落時の注意](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/xperf/processing)も参照。',
'不採用分はメタデータ・ログをchecks配下に残し、大きな生ファイル約29.6GBを削除した。failed-raw-cleanup.jsonに対象・容量を記録した。',
'計測コピーのwarm-up挿入位置は、追加された準備コマンドのタイマーと区別するため限定した。挿入処理は以前と同じで、製品ソースは変更していない（prepare-compat.json）。','',
'JP+ Multiple UnitsのNewGRF警告が出るセーブをそのまま使用。警告を抑制する変更はしていない。各実行のstderrと、測定フックによるerror pause解除の記録を保存している。',
'採用した4本のETL・新旧exe/PDBは作業ツリー外にも保存し、recording-backup.jsonに場所・SHA-256を記録した。固定入力は既存の同一ZIPを参照し、重複ZIPは作っていない。','',
f'[関数別差分CSV](measurements/{NAME}/function-before-after.csv)、[注目関数](measurements/{NAME}/focus.json)、[canonical照合](measurements/{NAME}/canonical-comparison.json)、[validate](measurements/{NAME}/validate-result.json)、[全検証](measurements/{NAME}/validation.json)。','',
'補正の再計算:','','```powershell',f'python -B utils/passenger_wpr_control.py docs/measurements/{NAME}/control-97cdc0ad docs/measurements/{NAME}','```','']
REPORT.write_text('\n'.join(lines),encoding='utf-8',newline='\n')
print(REPORT,flush=True)
