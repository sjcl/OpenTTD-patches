# 2d51a9ca: 逆引き索引の実マップ測定

`2d51a9ca09a01189a55659e6bfc0d39ea181866b` で依頼の3段階を実施した。
**正しさの検証は583,649グループに対して violations=0。
一方、alternate の走査時間はマスク14.719 → 索引35.470 µs/回で増加し、
WPR の67cbbf32比も補正後 +1.594748 CPU ms/tickだった。**
候補の訪問数は減ったが、この実マップでは索引の維持費で時間が増えている。
製品ソースは追加変更していない。

## 1. validate による正しさ

前回と同じ凍結セーブから `--hyperpath-filter-mode validate` で2,048 tick 実行した。
終了時の製品診断は以下。

```text
Passenger hyperpath lookup: rebuilds=30582; indexed_groups=2475339; validated_groups=583649; violations=0
```

採用された **583,649グループすべてについて索引候補にも存在することを確認**した。
validate・alternate・索引 Hyperpath の採用数は、67cbbf32 のマスク実行と同じ583,649。
対象 walk 数も全て144,211で一致した。
validate の判定自体は全走査で行うので、検証モードの時間を索引の性能値には使わない。

終了 canonical は validate・alternate・Legacy WPR・Hyperpath WPR の各24項目が、
67cbbf32 の同じモードと一致した（**計96/96、差分・欠落・追加0**）。
支払い valid、旅客・DB・所属・積載 digest、表の行数・論理 Work 等を含む。
64 bit digest は Python の整数で照合し、版情報を含むセーブのバイナリ SHA とは区別した。

[canonical 全96項目](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/canonical-all-runs.csv)、
[validate 原文](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/validate/on-1-console.txt.gz)。

## 2. alternate: 索引対マスク

`--hyperpath-filter-alternate` を1プロセスで1回実行した。
全走査との比較ではなく、同じ積み込みコンテキスト内で方式を固定した索引対マスクの比較。

```text
Passenger hyperpath walk time: filtered_us=1059101; filtered_groups=11624389; unfiltered_us=0; unfiltered_groups=0; indexed_us=2562997; indexed_groups=710634
```

| 方式 | walks | 合計 µs | 訪問 groups | µs/回 | groups/回 |
| --- | ---: | ---: | ---: | ---: | ---: |
| マスク / filtered | 71,953 | 1,059,101 | 11,624,389 | **14.719345** | 161.555 |
| 索引 / indexed | 72,258 | 2,562,997 | 710,634 | **35.470079** | 9.835 |

`unfiltered_walks=0` で、両方式が実際に使われていることを確認した。
索引は **20.750735 µs/回遅い**。走査頻度は144,211 / 2,048 = 70.415527回/tickなので、
全てを索引にした場合の走査区間の増加推定は **+1.461174 ms/tick**。
これは混在1回の平均からの外挿であり、tick 全体の実測差ではない。

訪問数は減っているが、索引のタイマーは `RefreshHyperpathLookup` の遅延更新も含む。
lookup 行は `rebuilds=15365; indexed_groups=1230198`。
割当先による負荷差や、索引更新の混在実行での償却のされ方までは分離していない。

[平均値 CSV](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/alternate-walks.csv)、
[全カウンタ・原文3行・計算結果](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/stages.json)。

## 3. WPR: tick 全体の補正比較

前回と同じ手順で Legacy と新しい既定の索引 Hyperpath を各2,048 tick 記録した。
WPR は `--hyperpath-filter-mode index` で既定方式を明示し、マスクを指定する
`--hyperpath-filter` は使っていない。
`enabled=false; mode=index` は索引方式で正常な表示。

指定どおり以下を実行し、標準出力を [control.json](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/control.json) に保存した。
分類語・2,048 tick・補正式は既定のままで変更していない。

```text
python -B utils/passenger_wpr_control.py docs/measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17 docs/measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17
```

以下は `StateGameLoop` がある主スレッドスタックの sampled CPU ms/tick。
同じモード同士の新旧差を取り、`machine_factor = 新 untouched / 旧 untouched`、
補正後の新時間は `新 / machine_factor`。正の差は時間増加。

| モード | 67cbbf32 | 今回 生値 | 生の差 | machine_factor | 今回 補正後 | 補正後の差 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Legacy | 24.974269 | 23.597858 | −1.376411 | 0.944422859 | 24.986538 | **+0.012269** |
| Hyperpath | 25.057370 | 25.511445 | +0.454075 | 0.957201425 | 26.652118 | **+1.594748** |

Hyperpath の補正後差の内訳は touched **+1.144413**、heap分類 **+0.450335**。
untouched の差は係数の定義により0になる。
Legacy は補正するとほぼ同じだった。
この補正は共通の速度変動を仮定した1ペアの推定であり、周波数の直接測定ではない。
スタック不明や `StateGameLoop` 外側のサンプルを推測でこの合計へ配分していない。

参考の simulation wall 平均は Legacy 26.362678→24.813136、
Hyperpath 26.132634→26.981592 ms/tick。上記 sampled CPU の補正値とは別の測定値。

### 索引の維持費

純粋な索引 Hyperpath の終了時カウンタ:

```text
Passenger hyperpath lookup: rebuilds=30582; indexed_groups=2475339; validated_groups=0; violations=0
```

全体再構築は **14.932617回/tick**、再索引グループの延べ数は **1,208.661621件/tick**。
walk time 行の `indexed_groups=1645937` は訪問数で、上の lookup 行とは異なる。
`rebuilds` は epoch / station / cargo の変更による全体再構築、lookup の `indexed_groups` は
宛先の dirty 更新も含む再索引量。グループの増減と行の公開・破棄による原因別の内訳は未計数。

| 保守関数（StateGameLoop 内） | inclusive CPU ms/tick | exclusive CPU ms/tick |
| --- | ---: | ---: |
| StationCargoList::RefreshHyperpathLookup | **2.030696** | 0.317381 |
| StationCargoList::IndexHyperpathGroup | 1.404236 | 0.366646 |
| PassengerServiceDatabase::HyperpathLookupCandidates | 0.872081 | 0.163560 |
| StationCargoList::UnindexHyperpathGroup | 0.008302 | 0.008302 |

これらは親子関数なので inclusive を加算しない。
いずれかを含むスタックの和集合は4,176サンプル、**生2.038999 / 補正後2.130167 CPU ms/tick**。
ほぼ `RefreshHyperpathLookup` 配下に集中している。
名前付き関数に帰属した範囲であり、全索引関連作業の厳密な総費用とは断定しない。

一方 `MayUseHyperpathPolicy` inclusive は **0.403870 → 生0.158204、補正後0.165277**、
補正後 **59.08%減（−0.238593 ms/tick）**だった。
判定側の削減は確認できるが、今回の索引保守費を吸収できていない。
次に見る場所は、`RefreshHyperpathLookup` の全体再構築頻度と、その配下の再索引・候補列挙。
今回は測定だけを行い、製品への追加修正は入れていない。

[保守スタックの和集合と関数別集計](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/maintenance.json)、
[MayUseHyperpathPolicy 新旧比較](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/membership-before-after.csv)、
[全 Hyperpath 関数の inclusive / exclusive](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/hyperpath-functions.csv)、
[Legacy 全関数](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/legacy-functions.csv)。

## 条件と検証

前回と同じセーブ、開始3181119581・終了3181121629。全実行で2,048 StateTicks、
precomputed ON、cold/warm、ロード済みバックグラウンド処理の完了待ち、主スレッド affinity mask 1。
途中の意図的な設定変更なし。以前の alternate にあった中間の transfer penalty 変更は今回は入れていない。
MSVC x64 Release、assert ON、LTO OFF、静的 CRT、`/Zi /Zo`、完全な対応 PDB。
Ryzen 9 9950X / Windows build 26200。
入力 SHA-256 は `306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。

**728テスト / 9,926,891 assertions 成功**。
WPR の Lost Events / Lost Buffers は両モードとも0、製品関数の未解決表記0。
Function 表と Stack 表の総 Weight・サンプル数は一致。
主スレッドのスタック不明は Legacy 688/55,894、Hyperpath 657/55,589。
四実行とも競合検出0、NewGRF 警告と計測用 error pause 解除は前回と同条件で原文保存した。

WPA の Invalid Event 警告は各1件。
時刻78.738153秒 / 78.207466秒は集計終了57.069秒 / 56.636秒より後。
両 export は exit 0、原文 CSV は gzip で保存し、展開後の SHA が原文と一致することを検証した。
生 ETL（約2.23 / 2.14 GB）、PDB、終了セーブは ignored の `build-wpr-2d51a9ca/` に保持した。

計測ツールには明示 index の受け入れと、比較時の on/index 変更を許可するオプションを追加した。
他の入力・設定・tick 数・canonical の一致検査は維持し、補正スクリプトは変更していない。
実際に使用したツールソースも証跡へ保存した。

[再現手順](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/README.md)、
[WPR 集計](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/summary.json)、
[ビルド・PDB・ツールの識別](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/build-identity.json)、
[検証結果](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/checks/validation.json)、
[証跡 SHA-256](measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17/evidence-sha256.json)。
