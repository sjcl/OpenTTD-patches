# Hyperpath boarding destination filter の on/off 実測

対象は `sjcl/OpenTTD-patches` の `merge/hyperpath-with-table-build`、
`544835fc59da1ec5e2d4e6793e9528107f2cb493`。
製品 `src` のツリーは `3d4a5f6107acae4d81f37108724842cb7ea2e60e` で、変更していない。

**ONは調査グループの84.77%を省略し、中間・終了状態と終了セーブを完全に維持した。
今回の実マップでは速度改善は確認できない。**
4試行をモード別に合わせた平均はOFF **32.153** → ON **32.630 ms/tick**（1.48%増）。
通常進行は両ペアともONが約4.3%遅く、表更新中はペアによって差の向きが逆転した。

## カウンターと正確性

以下は各2,048 tickあたりの値。OFFの2回同士、ONの2回同士でそれぞれ一致した。

| `passenger_journey_stats total` の項目 | OFF | ON |
| --- | ---: | ---: |
| `filtered_walks` | 0 | 143,512 |
| `unfiltered_walks` | 143,512 | 0 |
| `groups_checked` | 22,832,661 | 3,477,938 |
| `groups_skipped` | 0 | 19,354,723 |
| `groups_admitted` | 589,281 | 589,281 |

`ON.checked + ON.skipped = OFF.checked` が成立し、対象走査数・通過候補数も一致する。
除外率は `19,354,723 / 22,832,661 = 84.767706%`。
グループ数は走査中の累積機会であり、人数・一意の目的地数ではない。

4試行の中間と終了のcanonical各24項目がすべて一致した。
旅客Packet（容量不足履歴を含む）、DB保存状態、車両/駅への所属、積載・予約、
支払い、表の進行と論理Workを照合し、支払い検査もすべて正常。
さらに終了セーブ4ファイルのSHA-256まで一致した。

実輸送の診断値も全試行で一致する。`Schedule transport` は乗車289,835、
乗換133,611、配送127,877人。`Untimed transport` は乗車4、乗換202、配送102人。
容量不足記録は75,731 Packet / 138,499人分の機会で一致し、一意の人数とは解釈しない。

既存の `[hyperpath]` テストは **38ケース / 36,521 assertions**、
独立した線を持つ除外確認用の隠し合成テストも **1ケース / 16,982 assertions** が通過。
前者には4 seed、容量不足、保持/現行世代の混在、Legacyへの切替を含む
`Hyperpath boarding destination filter preserves every loading decision` が含まれる。
ログは [Hyperpath](measurements/passenger-hyperpath-filter-2026-10-17/checks/native-hyperpath.log) と
[合成テスト](measurements/passenger-hyperpath-filter-2026-10-17/checks/synthetic-filter.log)。

## 時間

単位はms/tick。各行は独立した同一入力からの2,048 tick。
Journeyは既存の内部タイマーで、simulation全体と包含関係にあるため加算しない。

| 実行順 | 全simulation平均 | 通常simulation平均 | 更新中simulation平均 | 全Journey平均 |
| --- | ---: | ---: | ---: | ---: |
| 1: off-a | 31.103 | 28.852 | 33.354 | 4.128 |
| 2: on-a | 33.719 | 30.094 | 37.345 | 4.573 |
| 3: on-b | 31.541 | 32.269 | 30.813 | 4.297 |
| 4: off-b | 33.203 | 30.930 | 35.477 | 4.328 |

モードごとの全標本を結合したsimulationの **平均 / p95 / p99**。
全区間は各モード4,096標本、通常・更新中は各2,048標本で、百分位同士の平均ではない。
平均差は `ON / OFF - 1`、正はONの時間増加。

| 区間 | OFF: 平均 / p95 / p99 | ON: 平均 / p95 / p99 | 平均差 |
| --- | ---: | ---: | ---: |
| 全区間 | 32.153 / 46.233 / 56.694 | 32.630 / 46.133 / 58.447 | +1.48% |
| 通常進行 | 29.891 / 43.588 / 56.607 | 31.181 / 41.302 / 61.164 | +4.32% |
| 表更新中 | 34.416 / 47.253 / 56.694 | 34.079 / 47.850 / 57.258 | -0.98% |

通常進行のペア別平均差は **+4.31% / +4.33%**。
全区間は **+8.41% / -5.01%**、表更新中は **+11.96% / -13.15%** と揺れる。
全Journey平均はOFF 4.228 → ON 4.435 ms/tick（4.89%増）。
グループ調査の削減は再現したが、それによる実ゲームの高速化は実証できなかった。
差の原因を分解する追加のプロファイラ測定は行っていない。

## 測定条件

同じ実行ファイル・入力から、OFF → ON → ON → OFF の順に各2,048 tickを実行する。
使用した実マップは既存測定で回収済みの `frozen/input.sav`、SHA-256
`306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
以前の原入力 `6783a500...` より24,576 tick後のセーブである。
今回の全試行は StateTicks `3181119581..3181121629` の同じ区間。

事前計算・Hyperpathを有効にし、保存済み表を破棄して初期表を停止中に構築する。
読み込み済みLinkGraphジョブの完了を待ち、次の順序で測定する。
ON試行では `hyperpath_filter_on` を使う。
設定コマンドの全内容は各試行の `scripts/game_start.scr` に保存している。

```text
passenger_journey_stats hyperpath_filter_off
passenger_journey_stats reset
… 2,048 tick進行 …
passenger_journey_stats total
```

前半1,024 tickは通常進行、直後に乗換コスト20→17で表の世代更新を起こし、
後半1,024 tickは保持表を使う更新中の進行を測る。
最後に停止中で更新表を完成させ、終了状態とセーブを採取する。
初期構築・終了後の構築・診断・保存はtick時間に含めない。
全プロセスのCPU時間とメモリ最大値にはそれらの準備・保存も含まれる。

MSVC 19.44 x64 Release、assert ON、LTO OFF、静的CRT。
既存の計測用コピーTUを使い、製品の経路選択・保存仕様・Work制限を変更していない。
主simulationスレッドはCPU mask 1に固定し、輸送追跡と乗車プロファイラを無効にした。
監視対象のOpenTTD・ビルド・テストの重複を検査する。
一般のOS負荷やCPU周波数を統制した実験ではなく、各モード2回の記述的比較である。
予定した4試行をすべて採用し、時間による除外・補正はしない。

入力の `JP+ Multiple Units` には既存の容量変更警告がある。
各回の計測用consoleでerror pauseを1回解除し、警告と解除を原文ログに保存する。
製品の警告・停止処理は変更していない。

## 証跡と再現

[再現手順](measurements/passenger-hyperpath-filter-2026-10-17/README.md)、
[検査済み集計](measurements/passenger-hyperpath-filter-2026-10-17/summary.json)、
[ソース・バイナリ識別](measurements/passenger-hyperpath-filter-2026-10-17/source-identity.json)、
[証跡のSHA-256](measurements/passenger-hyperpath-filter-2026-10-17/evidence-sha256.json)。
生のtick列・console原文・中間/終了状態・使用したコピーTUを保存し、
セーブ・バイナリ・NewGRF資産そのものは含めない。

後続の `594bbecad3` をMSVCでビルドした
[alternateモード1プロセス測定](passenger-hyperpath-filter-alternate-2026-10-17.md) では、
走査区間の差を約1.735 ms/tickの節約と推定した。
このページの4試行は変更前 `544835fc59` の測定結果として保持する。
