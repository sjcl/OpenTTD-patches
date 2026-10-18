from pathlib import Path
import json

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-rebaseline-88cb61e3-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
def load(p):return json.loads(p.read_text(encoding='utf-8'))
focus=load(OUT/'focus.json');validation=load(OUT/'validation.json');seeds=load(OUT/'seed-validation.json')
functions={(r['mode'],r['function'].removeprefix('openttd.exe!')):r for r in focus['functions']}
def row(mode,name,label):
    if (mode,name) not in functions:
        return f"| {label} | 0（0 samples） | 0（0 samples） | — | — | — |"
    r=functions[mode,name]
    return f"| {label} | {r['inclusive_old_ms_per_tick']:.6f} | {r['inclusive_new_ms_per_tick']:.6f} | {r['inclusive_raw_percent']:+.2f}% | {r['inclusive_corrected_ms_per_tick']:.6f} | {r['inclusive_corrected_percent']:+.2f}% |"
primary='\n'.join([
    row('hyperpath','PassengerPrecomputedTable::StepHyperpath','Hyperpath `StepHyperpath`'),
    row('hyperpath','PassengerServiceDatabase::TickPrecomputed','Hyperpath `TickPrecomputed`'),
    row('legacy','PassengerServiceDatabase::TickPrecomputed','Legacy `TickPrecomputed`')])
total='\n'.join(row(mode,'StateGameLoop',mode.capitalize()+' `StateGameLoop`') for mode in ['legacy','hyperpath'])
baseline=load(OUT/'baseline/summary.json');candidate=load(OUT/'candidate/summary.json')
wall='\n'.join(f"| {mode.capitalize()} | {baseline[mode]['simulation_wall_ms_per_tick']:.6f} | {candidate[mode]['simulation_wall_ms_per_tick']:.6f} |" for mode in ['legacy','hyperpath'])
control=focus['control']
backup=load(OUT/'input-backup.json')
rows=[]
for version in ['baseline','candidate']:
    for mode in ['legacy','hyperpath']:
        s=load(OUT/version/'summary.json')[mode]
        rows.append(f"| {version} / {mode} | {s['etl_bytes']:,} | {s['lost_events']} / {s['lost_buffers']} | {len(s['unresolved_product_functions'])} |")
quality='\n'.join(rows)
report=f'''# 88cb61e3: 別セーブから作り直した定常状態のWPR比較

新しい固定入力から8462e6cdでLegacy・Hyperpathを各8,192 tick定常化し、
各モードの同じ定常セーブを8462e6cdと88cb61e3で各2,048 tick実行した。
**終了canonicalは両モードとも新旧24/24完全一致**した。
64 bit digestを含む全項目をPythonの整数で比較した。

前回の入力本体は回収できなかったため、ユーザーの指示で別セーブに変更した。
今回の増減率は、この新入力で測り直した8462e6cdを基準にしている。
前回のHyperpath inclusive 2.954580420 / 3.010732959 ms/tickは別入力での参考値である。

## 指定された関数のinclusive

**この通常条件では表の構築Workが前後で不変で、`StepHyperpath`は新旧とも0サンプルだった。**
以下の`TickPrecomputed`は、構築を行わない区間の保守処理の時間である。
この区間の値から、`StepHyperpath`の高速化やLegacyの構築性能の悪化有無は判断できない。

すべて **CPU ms/tick**。`StateGameLoop`がスタックにあるサンプルを対象とし、
各スタック内の同じ関数は1回だけ数えた。親子関数のinclusiveはそれぞれの値であり、加算しない。

| 対象 | 8462e6cd | 88cb61e3 生値 | 生の変化 | 88cb61e3 補正後 | 補正後の変化 |
| --- | ---: | ---: | ---: | ---: | ---: |
{primary}

補正には既存の `utils/passenger_wpr_control.py` を既定分類のまま使った。
同モードのuntouchedサンプル時間の比 `新 / 旧` は
Legacy **{control['legacy']['machine_factor']:.9f}**、
Hyperpath **{control['hyperpath']['machine_factor']:.9f}**。
新しい時間をこの係数で割った値が補正後である。
untouchedを共通の速度変動の基準とする推定で、CPU周波数の直接測定ではない。

各条件1回のサンプリング測定であり、小さい差を性能改善の確証とは扱わない。
[関数別inclusive/exclusiveの新旧・補正値CSV](measurements/{NAME}/function-before-after.csv)、
[指定スクリプトの出力](measurements/{NAME}/control.json)、
[対象関数の数値JSON](measurements/{NAME}/focus.json)。

## tick全体

| 対象（CPU ms/tick） | 8462e6cd | 88cb61e3 生値 | 生の変化 | 88cb61e3 補正後 | 補正後の変化 |
| --- | ---: | ---: | ---: | ---: | ---: |
{total}

ドライバで実測したsimulation wall時間は次のとおり。ロード・停止中の表構築・保存・ETL書き出しを含まない。

| モード（wall ms/tick） | 8462e6cd | 88cb61e3 |
| --- | ---: | ---: |
{wall}

## 入力と正しさ

採用した元入力は手元の `SAVE2.sav`、SHA-256
`79fa148791f6abbf76f9b64852b475d2e937383d949413c4ffe55057f76c51c3`。
最初に試した `ga58d47238.sav` は `Invalid passenger OD graph revision` でロードできず、採用していない。
SAVE2はロード・再保存を確認した上で固定した。
前回の設定と資産246ファイルを復元して使用し、両モード・両版で共有した。

定常化はどちらも `13639680 → 13647872` の8,192 tick。
定常化と再ロードは状態確認専用で、時間データを性能比較には使用しない。
別ビルドが重なった観測や無効フラグも元のmetadataに保持している。

| モード | 凍結セーブのSHA-256 | 再ロード | 新旧WPR終了canonical |
| --- | --- | --- | --- |
| Legacy | `{seeds['legacy']['input_sha256']}` | 24/24一致 | 24/24一致 |
| Hyperpath | `{seeds['hyperpath']['input_sha256']}` | 24/24一致 | 24/24一致 |

WPRの4本はすべて `13647872 → 13649920`、実際に2,048 tick進んだことをドライバで確認した。
新旧の比較は同じモード同士で行った。LegacyとHyperpathそれぞれに独立した定常入力を用いている。
[canonical照合](measurements/{NAME}/canonical-comparison.json)、
[凍結・再ロード検証](measurements/{NAME}/seed-validation.json)、
[全検証結果](measurements/{NAME}/validation.json)。

## 記録・集計の手順

記録順は旧Legacy → 新Legacy → 旧Hyperpath → 新Hyperpath。
主スレッドaffinity mask 1、`--resume --precomputed --warm-precomputed --wait-background-ready`、
`--hyperpath-filter-mode index`、Legacyは`--no-hyperpath`、Hyperpathは`--hyperpath`。
ロード後の準備は停止中に完了させ、StateTicksが進まないことを確認してから記録した。

WPRは従来どおり `wpr -start CPU`、開始マーカー、2,048 tick、終了マーカー、`wpr -stop`。
ゲームは通常権限、記録ヘルパーだけ管理者権限で実行した。
終了済みビルドの孤立MSBuild待機ノードは、親の終了・子コンパイラなし・10秒間CPU使用なしを確認して終了した。
各記録前に60秒の静穏区間を置き、ETL内のkernel process lifetimeと開始・終了マーカーを照合した。
**4本とも記録区間に競合ビルド・テスト・別ゲームの重複はなかった。**

WPAExporterは前回と同じ11.7.395.48728。
[Microsoft公式ADK](https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install)を取得し、WPTを作業領域へ展開した。
各版の一致するPDBでCPU Usage (Sampled)のStacksとExclusive functionsをCSVに書き出した。

| 記録 | ETL bytes | Lost Events / Buffers | 製品関数の未解決サンプル |
| --- | ---: | ---: | ---: |
{quality}

全範囲（all / main / simulation_stack）で、Function表とStacks表の時間・サンプル総数の保存を確認した。
WPAの警告全文、不明スタックの量、区間監査結果はvalidationと各summaryに保存した。

| 版 | 関数別inclusive/exclusive | 集計・生CSVへの入口 |
| --- | --- | --- |
| 8462e6cd | [Legacy](measurements/{NAME}/baseline/legacy-functions.csv) / [Hyperpath](measurements/{NAME}/baseline/hyperpath-functions.csv) | [summary](measurements/{NAME}/baseline/summary.json) |
| 88cb61e3 | [Legacy](measurements/{NAME}/candidate/legacy-functions.csv) / [Hyperpath](measurements/{NAME}/candidate/hyperpath-functions.csv) | [summary](measurements/{NAME}/candidate/summary.json) |

各版の `legacy/`・`hyperpath/` にWPAの生CSVをgzipで保存し、展開後のSHAが元のCSVと一致することも検証した。
関数別CSVには全スレッド・主スレッド・simulation_stackの各範囲を含む。
全スレッド・主スレッドのexclusiveはWPA Function列、simulation_stackのexclusiveはスタック末端の値である。

## ビルドと保管

MSVC 19.44 x64、Release `/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert有効、LTO無効、static CRT、
`/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO`。
8462e6cdは独立したソースアーカイブからビルドし、srcの1,869ファイルをGitの内容と照合した（Windowsの改行変換を除いて一致）。
728 tests / 9,930,179 assertionsが成功。
88cb61e3にはWindows SDKの`far`マクロ衝突を避けるメンバー名変更だけを加えている（`b7cdf719e8`）。
この版の729 tests / 10,074,203 assertionsも成功した。
[ビルド識別情報・exe/PDB SHA](measurements/{NAME}/build-identities.json)。

元入力・定常セーブ2本・設定・全資産は、worktreeとは別の場所にも検証付きZIPとして保存した。
バックアップは257エントリ、{backup['bytes']:,} bytes、SHA-256 `{backup['sha256']}`。
場所と復元方法は [input-backup.json](measurements/{NAME}/input-backup.json) とZIP内の `RESTORE.txt` に記録した。
生ETLと一致するPDBもworktree外へ保管した。[保管先とSHA](measurements/{NAME}/recording-backup.json)。

再実行・書き出し・検証スクリプトは [checks](measurements/{NAME}/checks) に保存した。
今回のWPR生成物はローカルの `build-rebaseline-88cb61e3/` にあり、セーブ・資産・exe・PDB・生ETLそのものはGitに含めていない。
'''
(ROOT/'docs'/(NAME+'.md')).write_text(report,encoding='utf-8')
print('Rendered report:',ROOT/'docs'/(NAME+'.md'))
