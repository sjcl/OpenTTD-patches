# 8462e6cd: 実マップの索引検証・A/B・WPR

対象は `8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4`、製品 src ツリーは
`631f498ec2e22d2910bb414d7f3b2f9b780b9e8e`。
比較元は [67cbbf32 の WPR](../passenger-hyperpath-wpr-67cbbf32-2026-10-17/README.md)。
製品ソースは編集せず、前回と同じ MSVC x64 Release・assert ON・LTO OFF・静的 CRT、
`/O2 /Ob2 /DNDEBUG /Zi /Zo`、リンク
`/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO` で独立ビルドした。

```text
python -B utils/passenger_estimated_prepare.py SOURCE BUILD 8462e6cd --cpu
python -B utils/passenger_wpr_prepare.py BUILD
python -B utils/passenger_hyperpath_build.py BUILD BUILD/build.log --target openttd openttd_test
BUILD/Release/openttd_test.exe --reporter compact
```

同じ凍結セーブから、validate → alternate → Legacy WPR → Hyperpath WPR の順に各2,048 tick。
入力 SHA-256 は `306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
開始3181119581、終了3181121629。全実行で precomputed / cold / warm / wait-background-ready、
主スレッド affinity mask 1、途中の意図的な設定変更なし。
旧 alternate 測定にあった中間の transfer penalty 変更は今回は入れていない。

共通の実行形は次の通り。実際の全引数は各実行の `process.json` と `checks/measure.py` に保存する。

```text
python -B utils/passenger_precomputed_real_map.py run BUILD/Release/openttd.exe FROZEN BUILD/NAME --ticks 2048 --timeout 1800 --measurement-wait 120 --precomputed HYPERPATH_SWITCH --cold-precomputed --warm-precomputed --wait-background-ready --main-thread-affinity-mask 1 --lang BUILD/lang MODE_SWITCHES
```

| NAME | HYPERPATH_SWITCH | MODE_SWITCHES |
| --- | --- | --- |
| validate | --hyperpath | --hyperpath-filter-mode validate |
| alternate | --hyperpath | --hyperpath-filter-alternate |
| legacy | --no-hyperpath | --hyperpath-filter-mode index --wpr-profile |
| hyperpath | --hyperpath | --hyperpath-filter-mode index --wpr-profile |

WPR の index は新しい既定と同じ方式を明示指定したもの。
`--hyperpath-filter` はマスク ON を意味するため、今回の WPR には指定していない。
`enabled=false; mode=index` は索引方式で正常な表示。既存フィールドの意味を変更していない。
validate は全走査の判定結果を使い、採用したグループが索引候補にもあるかを検査する。
`violations=0` と `validated_groups>0`、終了 canonical の旧版との一致を検査する。

alternate はコンテキストごとに index / mask を割り当てる。
`indexed_us / indexed_walks` と `filtered_us / filtered_walks`、各 groups/walk を比較する。
`stages.json` は終了時の原文3行と全カウンタを保持し、`alternate-walks.csv` は計算した平均。
全走査を索引にした場合の走査区間の節約推定は
`(filtered_us/filtered_walks - indexed_us/indexed_walks) * (filtered_walks+indexed_walks)/2048/1000`。
これは混在1回からの外挿で、tick 全体の改善量ではない。
タイマーは索引側の `RefreshHyperpathLookup` を含み、他の候補統合等は含まない。

lookup 行の `rebuilds` は epoch / station / cargo 変更、または変更ログを追い切れない場合の全体再構築回数。
同じ行の `indexed_groups` は dirty な宛先について再索引したグループの延べ数であり、
行の公開・破棄による差分更新や、新規グループによる更新も含む。独立なグループの個数ではない。
walk time 行の `indexed_groups` は索引走査で訪問したグループ数で、lookup 行とは区別する。

WPR ヘルパーだけを管理者起動し、ゲームは通常権限。準備ゲートから `wpr -start CPU`、
開始マーカー、2,048 tick 後の終了マーカー、`wpr -stop NAME/cpu.etl` を実行する。
終了集計とセーブは記録区間外。前回と同じ WPA プロファイル、Windows symcache、対応する新 PDB を使う。

```text
python -B utils/passenger_wpr_export.py BUILD TOOLKIT --output-name wpa --local-symbols-only
python -B utils/passenger_wpr_summarize.py BUILD EVIDENCE --export-name wpa
python -B utils/passenger_wpr_compare.py docs/measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17 EVIDENCE --allow-hyperpath-filter-change
python -B utils/passenger_wpr_control.py docs/measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17 EVIDENCE
python -B EVIDENCE/checks/membership.py docs/measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17 EVIDENCE
```

前回までに更新済みの export / compare をそのまま使った。export は明示 index を許可する。
compare の `--allow-hyperpath-filter-change` は on/index 間の変更を明示許可するが、
他の設定・入力・tick 数と canonical の一致は引き続き必須である。
補正スクリプトの分類・計算式は変更していない。出力は `control.json`。
untouched の新/旧比を machine_factor とし、新時間をこの係数で割って旧時間との差を取る。
これは各処理が共通の割合で速度変動するという仮定での推定であり、wall タイマーとは区別する。

`maintenance.json` は主スレッドの StateGameLoop 内で、RefreshHyperpathLookup / IndexHyperpathGroup /
UnindexHyperpathGroup / HyperpathLookupCandidates のいずれかを含むスタックの和集合。
inclusive の親子を重複加算していない。名前付き関数に帰属したサンプルの範囲であり、
すべての索引関連作業の厳密な総費用とは断定しない。
関数別の inclusive / exclusive と、その union の生値・補正値を保持する。

各モードの原文 WPA CSV は gzip、関数別集計は `*-functions.csv`。
exclusive は all/main では WPA Function 列、simulation_stack ではスタック leaf。
canonical は24項目を Python の整数精度で比較する。版情報が入るセーブのバイナリ SHA は判定に使わない。
生 ETL・対応 PDB・セーブと資産は ignored の `build-wpr-8462e6cd/` に保持する。
