# 8462e6cd: 索引を tick 間で保持する修正の実マップ測定

`8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4` で、前回と同じ3段階を実施した。
**validate は延べ583,649件の採用グループに対して violations=0。
alternate はマスク14.607 → 索引9.127 µs/回となり、索引が37.5%短かった。
WPR の67cbbf32比は補正後 −0.592802 CPU ms/tickだった。**
正しさを確認でき、走査部分と指定の補正後 tick 時間に短縮が見られた。
製品への追加修正は行っていない。

## 1. validate

同じ凍結セーブから `--hyperpath-filter-mode validate` で2,048 tick 実行した。
終了時の製品診断:

```text
Passenger hyperpath lookup: rebuilds=3299; indexed_groups=157966; validated_groups=583649; violations=0
```

**延べ583,649件の採用グループすべてが索引候補にも存在**した。
前回のような毎 tick の全体再構築が大幅に減った状態で、区間内の取りこぼしは0。
validate は全走査の判定結果を使うので、その実行時間は索引の性能値には使わない。

validate・alternate・索引 Hyperpath の walk 数は144,211、採用数は583,649で、
67cbbf32 のマスク実行と一致した。
終了 canonical も各実行24項目が67cbbf32の同じモードと一致。
Legacy WPR を含む四実行で **96/96一致、差分・欠落・追加0**。
支払い valid、旅客・DB・所属・積載 digest、表の行数・論理 Work 等を Python の整数精度で照合した。
版情報を含むセーブのバイナリ SHA とは区別する。

[全96項目の照合](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/canonical-all-runs.csv)、
[validate 原文](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/validate/on-1-console.txt.gz)。

## 2. alternate: 索引対マスク

`--hyperpath-filter-alternate` を1プロセスで1回実行した。
終了時の walk time 行:

```text
Passenger hyperpath walk time: filtered_us=1051028; filtered_groups=11624389; unfiltered_us=0; unfiltered_groups=0; indexed_us=659510; indexed_groups=710634
```

| 方式 | walks | 合計 µs | 訪問 groups | µs/回 | groups/回 |
| --- | ---: | ---: | ---: | ---: | ---: |
| マスク / filtered | 71,953 | 1,051,028 | 11,624,389 | **14.607146** | 161.555 |
| 索引 / indexed | 72,258 | 659,510 | 710,634 | **9.127155** | 9.835 |

`unfiltered_walks=0`、両方式の walks が正であることを確認した。
索引は **5.479991 µs/回（37.52%）短い**。
全走査頻度は144,211 / 2,048 = 70.415527回/tickなので、全走査を索引にした場合の
走査区間の節約推定は **0.385876 ms/tick**。
混在実行の計時合計は0.835224 ms/tickだった。

この推定は1回の混在実行からの外挿であり、tick 全体の実測差ではない。
索引側のタイマーには `RefreshHyperpathLookup` の遅延更新も含む。
コンテキスト内の方式は固定し、コンテキスト内のメモ化を保っている。
lookup 行は `rebuilds=2778; indexed_groups=130874`。

[平均値 CSV](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/alternate-walks.csv)、
[全カウンタと計算結果](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/stages.json)。

## 3. WPR: 67cbbf32 比の補正

Legacy と純粋な索引 Hyperpath を前回と同じ手順で各2,048 tick 記録した。
`--hyperpath-filter-mode index` で新しい既定方式を明示した。
`enabled=false; mode=index` は索引方式で正常な表示であり、マスク OFF の意味ではない。

指定のコマンドを既定の分類・補正式のまま実行した。
標準出力は [control.json](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/control.json) に保存する。

```text
python -B utils/passenger_wpr_control.py docs/measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17 docs/measurements/passenger-hyperpath-index-8462e6cd-2026-10-17
```

以下は主スレッドの `StateGameLoop` を含むスタックの sampled CPU ms/tick。
同じモード同士の新旧差を取り、`machine_factor = 新 untouched / 旧 untouched`、
補正後の新時間は `新 / machine_factor`。負の差は短縮。

| モード | 67cbbf32 | 今回 生値 | 生の差 | machine_factor | 今回 補正後 | 補正後の差 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Legacy | 24.974269 | 24.276790 | −0.697479 | 0.974268895 | 24.917957 | **−0.056312** |
| Hyperpath | 25.057370 | 27.759817 | +2.702447 | 1.134694758 | 24.464568 | **−0.592802** |

Hyperpath の補正後差は touched −0.585699、heap分類 −0.007103 ms/tick。
untouched の補正後差は係数の定義により0となる。
今回は Hyperpath の untouched 自体が旧版より **13.47%長く**、生値と補正後値で差の符号が逆転する。
したがって、約0.593 ms/tickの短縮は指定の補正モデルでの推定として扱う。
各処理が共通の割合で変動するという仮定であり、周波数を直接測定したものではない。
1ペアだけで全環境での短縮量までは確定しない。

製品の simulation wall 平均は Legacy 26.362678→25.551710、
Hyperpath 26.132634→29.836145 ms/tickだった。上記 sampled CPU の補正値とは別の測定値。
スタック不明や `StateGameLoop` の外側を推測で補正合計へ配分していない。

## 再構築と残る保守費

純粋な索引 Hyperpath の終了時診断:

```text
Passenger hyperpath lookup: rebuilds=3299; indexed_groups=157966; validated_groups=0; violations=0
```

| 実行 | 2d51a9ca rebuilds | 今回 rebuilds | 2d51a9ca 再索引 groups | 今回 再索引 groups |
| --- | ---: | ---: | ---: | ---: |
| validate | 30,582 | 3,299 | 2,475,339 | 157,966 |
| alternate | 15,365 | 2,778 | 1,230,198 | 130,874 |
| 索引 Hyperpath WPR | **30,582** | **3,299** | **2,475,339** | **157,966** |

純粋な索引方式では全体再構築 **89.21%減**、再索引グループの延べ数 **93.62%減**。
1 tick 当たりはそれぞれ1.610840回、77.131836件になった（前回14.932617回、1,208.661621件）。
`rebuilds` は epoch / station / cargo の変更や変更ログを追い切れない場合の全体再構築。
駅・原因ごとの内訳までは計数していない。
lookup 行の `indexed_groups` は新規グループ・公開/破棄された行に対する更新を含む再索引量で、
walk time 行の訪問数 `indexed_groups=1645937` とは区別する。

| 今回の保守関数（StateGameLoop 内） | inclusive CPU ms/tick | exclusive CPU ms/tick |
| --- | ---: | ---: |
| StationCargoList::RefreshHyperpathLookup | 0.181664 | 0.013179 |
| StationCargoList::IndexHyperpathGroup | 0.164090 | 0.027840 |
| PassengerServiceDatabase::HyperpathLookupCandidates | 0.117693 | 0.015629 |
| StationCargoList::UnindexHyperpathGroup | 0.013668 | 0.010740 |

親子の inclusive は加算しない。いずれかを含むスタックの和集合は392サンプル、
**生0.191426 / 補正後0.168703 CPU ms/tick**。
前回2d51a9ca の同じ範囲は生2.038999 / 補正後2.130167だった。
両方を67cbbf32の尺度へ補正した参考比較で、保守費の大幅な減少が見られる。
名前付き関数のサンプルに帰属した範囲であり、全索引関連作業の厳密な総費用とは断定しない。

`MayUseHyperpathPolicy` inclusive は67cbbf32の0.403870から、生0.231943 / 補正後0.204410へ減少。
補正後49.39%減（−0.199461 ms/tick）だった。

[再構築・再索引の新旧 CSV](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/lookup-before-after.csv)、
[保守スタックの和集合](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/maintenance.json)、
[全 Hyperpath 関数の inclusive / exclusive](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/hyperpath-functions.csv)、
[Legacy 全関数](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/legacy-functions.csv)、
[MayUseHyperpathPolicy 新旧比較](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/membership-before-after.csv)。

## 条件と証跡の検証

四実行とも開始3181119581、終了3181121629の2,048 StateTicks。
同じセーブ、precomputed ON、cold/warm、ロード済みバックグラウンド処理の完了待ち、
主スレッド affinity mask 1、途中の意図的な設定変更なし。
MSVC x64 Release、assert ON、LTO OFF、静的 CRT、`/Zi /Zo` と完全な対応 PDB。
Ryzen 9 9950X / Windows build 26200。
入力 SHA-256 は `306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。

**728テスト / 9,930,179 assertions 成功**。
WPR の Lost Events / Lost Buffers は両方0、製品関数の未解決表記0。
Function 表と Stack 表の総 Weight・サンプル数は一致した。
主スレッドのスタック不明は Legacy 571/57,419、Hyperpath 698/61,466。
四実行の競合検出0、NewGRF 警告と計測用 error pause 解除は前回と同条件で原文に保存した。

WPA の Invalid Event 警告は各1件。時刻80.197943秒 / 84.556508秒は、
集計終了58.366秒 / 62.764秒より後だった。両 export は exit 0。
原文 CSV は gzip で保存し、展開後の SHA が export 原文と一致することを確認した。
生 ETL（約2.25 / 2.74 GB）、PDB、終了セーブは ignored の `build-wpr-8462e6cd/` に保持する。
計測ツールは前回から変更せず、実際に使用したソースも証跡へ保存した。

[再現手順](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/README.md)、
[WPR 集計](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/summary.json)、
[ビルド・PDB・ツール識別](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/build-identity.json)、
[検証結果](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/checks/validation.json)、
[証跡 SHA-256](measurements/passenger-hyperpath-index-8462e6cd-2026-10-17/evidence-sha256.json)。
