# Legacy / Hyperpath をそれぞれ定常化した WPR 比較

**Legacyも元入力から8,192 tick進めて別のセーブを凍結し、追加WPRを取得した。**
Legacyの準備実行・追加2,048 tickとも Hyperpath索引のwalk / rebuildは0。
各方式の保存・再ロードcanonicalは **24/24一致、合計48項目一致**。
対象製品は `8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4`、実施日は2026-10-18 JST。

StateGameLoopの sampled CPU は **Legacy 26.096451 / Hyperpath 26.785369 ms/tick**、
差は **+0.688917 ms/tick**だった。
DB処理・待客イベント・積卸しの3範囲では **+3.321722 ms/tick** 増え、その他の範囲の
−2.632805 ms/tickと相殺されている。主な増加はテーブル構築と積卸しにある。
製品の追加最適化は行っていない。

## 入力と保存・再ロード

共通の元入力 SHA-256:

```text
306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629
```

両方式ともこの元入力をそれぞれの設定で3181119581 → 3181127773の8,192 StateTicks実行した。
LegacyをHyperpathの終了セーブから切り替える手順を避け、両方式の保存tickを揃えた。
経路選択・輸送の履歴が異なるため、二つの保存内容は異なる。

| 凍結入力 | SHA-256 | bytes |
| --- | --- | ---: |
| Legacy | `b276c5de400651f646c129ab394bc97a686f38aa345b3a4a799e07c8a06289ca` | 39,109,276 |
| Hyperpath | `275544f1fd7bb16920ebd325c3a916ee06f164fcbe53abf0c1bb6409e9802f9f` | 39,062,116 |

ローカルpackage（`input.sav`は読み取り専用）:

```text
Legacy:    C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-passenger-legacy-steady-8462e6cd/frozen-8192
Hyperpath: C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-passenger-steady-8462e6cd/frozen-8192
```

Legacyを `--ticks 0 --resume` で再ロードし、tickを進めず保存した。
packet / payment / database / ownership / loading digest、行数、論理Work等を含む
canonical全24項目が一致し、支払いはvalid。
Hyperpathの先ほどの24項目一致と合わせ、48項目をPythonの整数精度で照合した。
各方式内の再ロードを照合しており、方式間の状態一致を要求していない。
元入力、設定、全アセットのSHAも検証した。

[Legacy凍結の由来](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/frozen-seed.json)、
[全アセットのハッシュ](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/frozen-legacy/package.json)、
[48項目の照合](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/reload-canonical.csv)。

## WPRの条件と結果

追加LegacyはこのLegacy定常入力から、Hyperpathは先ほどのHyperpath定常入力の記録を再利用した。
再利用したHyperpathの原文CSV・診断・状態と集計結果は、先ほどの証跡とバイト同一。
由来とSHAを [hyperpath-reuse.json](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/hyperpath-reuse.json) に記録した。

両方式とも **3181127773 → 3181129821の2,048 StateTicks**。
`--resume`、precomputed ON、filter mode=index、coldリセットなし。
保存済みのモードを保ち、paused warmとbackground-ready待ちを済ませてから診断をresetした。
主スレッドaffinity mask 1、EXE/PDB・MSVCのビルド条件は同一。
記録時刻はHyperpathが00:02:50〜00:03:49、Legacyが01:04:00〜01:05:05 JST。

| 指標 | Legacy | Hyperpath | Hyperpath − Legacy |
| --- | ---: | ---: | ---: |
| StateGameLoop sampled CPU ms/tick | 26.096451 | 26.785369 | **+0.688917** |
| simulation wall ms/tick | 27.585556 | 28.405671 | **+0.820115** |

主スレッドのStateGameLoop外はLegacy 5.416417 / Hyperpath 1.881671 CPU ms/tick。
この範囲はtick処理の差に割り当てていない。全スレッド・主スレッドの数値もCSVへ保存した。
入力が異なるため旧入力との `passenger_wpr_control.py` による補正は行っていない。
同一ホストの1ペアの生値であり、CPU周波数や方式ごとの輸送状態の違いもあるので、
約0.7 msの総差が普遍的な値とは断定しない。
今後の変更は各方式の同じ凍結入力から実行し、同じ方式のcanonicalを確認して比較する。

### 既存Hyperpath意図の影響が消えたこと

| 診断 / 関数 | 前回のHyperpath入力から切替えたLegacy | 今回の独立定常Legacy | Hyperpath |
| --- | ---: | ---: | ---: |
| lookup rebuilds | 2,984 | **0** | 2,971 |
| lookup indexed_groups（保守量） | 136,833 | **0** | 140,186 |
| indexed_walks | 139,368 | **0** | 140,375 |
| MayUseHyperpathPolicy inclusive CPU ms/tick | 1.476260 | **0（サンプル0）** | 0.423400 |
| migration packets | 0 | **0** | 0 |

Legacyの8,192 tick準備実行でも上記の索引カウンタは0だった。
前回の共通Hyperpath入力ペアに含まれた既存意図の処理を、今回のLegacy計測から除けた。
独立定常LegacyとHyperpathではgeneration原因の処理が543,325 / 1,414,511件あり、
通常の世代更新等は引き続き含む。

[全診断・512 tickごとの時間](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/stages.json)、
[原因別件数と時間](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/scheduler-causes.csv)、
[前回Legacyとの参考比較](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/legacy-input-comparison.csv)。

## 差がある処理

以下はStateGameLoopを含む主スレッドのスタックのinclusive sampled CPU ms/tick。

| 処理範囲 | Legacy | Hyperpath | 差 |
| --- | ---: | ---: | ---: |
| PassengerServiceDatabase::Tick | 1.208131 | 3.022997 | **+1.814866** |
| RunPassengerWaitingEvents | 1.972930 | 2.257914 | **+0.284983** |
| LoadUnloadVehicle | 1.330046 | 2.551918 | **+1.221872** |
| 上記3範囲の和集合 | **4.511107** | **7.832829** | **+3.321722** |
| その他のStateGameLoop | 21.585345 | 18.952540 | −2.632805 |

実際のスタックで、この3関数のうち同時に含まれるものは最大1つだった。
和集合と各関数の合計が一致することを検証し、親子inclusiveの二重計上を避けた。
LoadUnloadVehicleは全貨物を含む範囲であり、この3範囲を旅客処理すべての厳密な定義とはしていない。

その他の範囲ではRunTileLoopが −1.512863 ms/tick。
CallVehicleTicks全体の差は +0.176166なので、積卸し以外の車両処理では約−1.046 ms/tickとなる。
方式ごとの輸送履歴や実行時の変動もあるため、これらを共通の機械係数として補正していない。

[和集合の検証](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/component-union.json)、
[StateGameLoop直下の全内訳](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/branch-diff.csv)。

### 次の調査対象

最大の増分はテーブル構築。
`TickPrecomputed` はLegacy 1.194898 / Hyperpath 3.010733 ms/tick（+1.815835）だった。

| 関数 | Legacy exclusive | Hyperpath exclusive | Hyperpath inclusive |
| --- | ---: | ---: | ---: |
| PassengerPrecomputedTable::StepHyperpath | 0 | **1.328931** | **2.954580** |
| PassengerPrecomputedTable::SetSearchLabel | 0.089921 | **0.530400** | 0.530400 |
| PassengerPrecomputedTable::MarkBoardingCandidate | 0 | 0.149193 | 0.150167 |
| PassengerJourneyLoadingContext::MayUseHyperpathPolicy | 0 | 0.063490 | 0.423400 |
| StationCargoList::RefreshHyperpathLookup | 0 | 0.019534 | 0.166526 |

次は **StepHyperpathの内側、特にラベル更新・キュー操作の内訳**を詳しく調べる候補になる。
積卸しの+1.222 ms/tickも残り、そのうちMayUseHyperpathPolicyのinclusiveは0.423 ms/tick。
これらはスタック上の親子関係があるため単純には合算しない。
今回の証拠は、走査やLocalPolicyの代用を推測で変更するより、構築側の具体的な費用を先に調べる根拠になる。

[Legacy全関数](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/legacy-functions.csv)、
[Hyperpath全関数](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/hyperpath-functions.csv)、
[全関数inclusive / exclusive差](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/function-diff.csv)、
[差の上位各20件](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/top-function-differences.csv)。

## 記録区間の検証と計測ツールの修正

追加Legacyの最初の二試行は採用しなかった。一つ目は別ビルドが記録中に始まった。
二つ目ではETL内に44.520 msのCMake設定実行があり、10秒間隔の監視では拾えなかった。
両試行の診断・状態とETLのハッシュを保存し、容量確保のため無効ETL本体だけを削除した。

最終試行は、計測終了後のETL書き出し中に別OpenTTDの起動を11回観測した。
raw metadataは **`whole_process_timing_valid=false` / `simulation_timing_valid=true`** のまま保持する。
全活動が `after_simulation` で、ETLのプロセス寿命とbegin/endマーカーの独立照合でも、
**両方式とも記録区間内のビルド・テスト・他のゲームの重なり0**だった。
外側controllerの全プロセス競合0のassertは通らなかったが、runner・ゲームはexit 0。
[採用時の検証と理由](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/checks/measurement-complete.json) を別に保存し、元データを書き換えていない。
Legacyの終了canonicalは無効二試行とも24項目ずつ、合計48項目一致した。

計測ツールには次の修正を加えた。製品コード・EXE/PDBは変更していない。

- recorderに単独記録用の `-Mode legacy` を追加。既定値は従来どおり両モード。
- runnerは `wpr-finished.txt` がある区間を計測後に分類する。サンプルJSONはETL書き出し後に作られるため、それだけを待つと誤分類になる。
- WPA書き出し前にETLのプロセス寿命を検査する。約45 msの重なりを持つ実記録を拒否し、受理した二記録は通過した。

Lost Events / Lost Buffersは両方0、製品関数の未解決表記0。
Function / Stack表の総Weight・件数は全スレッド / 主スレッド / StateGameLoopの全範囲で一致。
主スレッドのスタック不明はLegacy 763/64,521、Hyperpath 671/58,693。
WPA Invalid Event警告は各1件で、87.251620秒 / 80.993506秒は集計終了65.568秒 / 59.667秒より後だった。
両exportはexit 0、原文CSVとgzip展開後のSHAはすべて一致。

同一バイナリの前回728テスト / 9,930,179 assertions成功を引き継ぎ、今回は再ビルドしていない。
セーブ・アセット・EXE/PDB・生の正常ETLはローカルに保持する。
Legacy ETLは約2.95 GB、再利用Hyperpath ETLは約4.52 GB。

[再現手順](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/README.md)、
[WPR集計](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/summary.json)、
[ETL区間検査](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/checks/legacy-window-audit.json)、
[全検証結果](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/checks/validation.json)、
[ビルドの由来](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/build-identity.json)、
[証跡SHA-256](measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18/evidence-sha256.json)。
