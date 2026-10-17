# Hyperpath 8,192 tick 後の凍結セーブと WPR 新基準

2026-10-18追記: Legacyも元入力から8,192 tick進めた追加計測は
[両モードをそれぞれ定常化した比較](passenger-routing-steady-pair-8462e6cd-2026-10-18.md)を参照。
以下はHyperpathの定常セーブを共通入力にした最初のペアの記録。

`8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4` で元入力を Hyperpath / index のまま
**8,192 tick 実行して、新しい計測入力を凍結した。再ロードの canonical は24/24一致。**
この入力から追加2,048 tick 実行した Hyperpath の診断は
**`cause=migration; packets=0; microseconds=0`** だった。
実施日は2026-10-17〜18 JST。製品ソースの追加変更はない。

同じ凍結入力から Legacy / Hyperpath の WPR も各2,048 tick 取得した。
ただし Legacy 側にも保存済み HyperpathIntent の処理が入る。
今回のペアは **共通の Hyperpath 入力から設定を切り替える比較の新基準**であり、
両方式をそれぞれ定常化した場合の差とは区別する。

## 凍結入力

| 項目 | 値 |
| --- | --- |
| 元入力 SHA-256 | `306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629` |
| 準備実行の StateTicks | 3181119581 → **3181127773**（8,192 tick） |
| 新入力 SHA-256 | `275544f1fd7bb16920ebd325c3a916ee06f164fcbe53abf0c1bb6409e9802f9f` |
| 新入力サイズ | 39,062,116 bytes |
| 設定 | Hyperpath / precomputed ON、filter mode=index |

セーブとアセットを含むローカル package は以下。
`input.sav` は読み取り専用にし、再実行は別の出力 directory に保存する。

```text
C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-passenger-steady-8462e6cd/frozen-8192
```

元入力を保持したまま、準備実行の終了セーブを新しい package にコピーした。
セーブ・設定・全アセットのハッシュを [package.json](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/frozen/package.json) に保存している。
実ファイルはローカルに保持し、Git にはハッシュ、状態、ログと再現手順を保存する。

新入力を `--ticks 0 --resume` でロードし、tick・設定・構築カーソルを進めず再保存した。
packet / payment / database / ownership / loading digest、行数、論理 Work 等を含む
canonical **24項目すべてが一致**し、支払いは valid。
64 bit の digest は Python の整数で照合した。
準備実行・再ロードとも競合検出0。元入力の SHA も変わっていない。

[凍結の由来](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/frozen-seed.json)、
[24項目の再ロード照合](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/reload-canonical.csv)、
[検証結果](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/checks/validation.json)。

## 移行処理の確認

| 実行 | tick数 | migration packets | migration µs |
| --- | ---: | ---: | ---: |
| 旧入力からの今回の準備実行 | 8,192 | 205,666 | 823,477 |
| 新入力からの Hyperpath WPR | 2,048 | **0** | **0** |
| 新入力からの Legacy WPR | 2,048 | 0 | 0 |

準備実行の移行件数は、前回の旧入力からの2,048 tick と同じ205,666件だった。
新入力の Hyperpath では追加区間の移行処理がなくなった。
一方、generation 原因の処理は Legacy 531,081件 / Hyperpath 1,414,511件あり、
世代更新や期限切れ等の通常処理は引き続き計測に含む。
この追加区間で移行0を確認したことと、あらゆる更新処理が消えることは区別する。

[原因別件数・時間](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/scheduler-causes.csv)、
[全診断と512 tickごとの時間](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/stages.json)。

## 同じ新入力からの WPR

両モードとも **3181127773 → 3181129821 の2,048 StateTicks**。
`--resume` で設定を引き継ぎ、`--cold-precomputed` は使っていない。
計測前の paused warm、background-ready 待ち、診断 reset、主スレッド affinity mask 1 は共通。
Legacy のモード切替 callback とテーブル準備は計測区間の外側で行った。
MSVC Release の EXE / PDB は前回と同一で、ビルド条件・計測 TU も引き継いだ。

| 範囲 / 指標 | Legacy | Hyperpath | Hyperpath − Legacy |
| --- | ---: | ---: | ---: |
| StateGameLoop sampled CPU ms/tick | 27.266678 | 26.785369 | **−0.481310** |
| simulation wall ms/tick | 28.807200 | 28.405671 | **−0.401529** |
| 主スレッド全体 sampled CPU ms/tick | 31.642112 | 28.667040 | −2.975073 |
| 主スレッドの StateGameLoop 外 sampled CPU ms/tick | 4.375434 | 1.881671 | −2.493763 |

StateGameLoop 外のサンプルを tick 処理の差に割り当てていない。
旧入力との tick 区間・輸送状態・処理量が異なるため、
`passenger_wpr_control.py <旧入力の証跡> <新入力の証跡>` による補正は行わない。
今後はこの入力と手順を固定し、同じモードの終了 canonical を確認して比較する。
今回の1ペアだけで数百µsの差の再現性までは確定しない。

### Legacy 側の既存 Hyperpath 意図

`passenger_journey_hyperpath=false` にしても、保存された HyperpathIntent は計測中に処理される。
`MayUseHyperpathPolicy` は packet の intent kind を見て判定する
（[測定した製品ソース](https://github.com/sjcl/OpenTTD-patches/blob/8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4/src/passenger_journey_active.cpp#L1429)）。

| 診断 | Legacy | Hyperpath |
| --- | ---: | ---: |
| lookup rebuilds | 2,984 | 2,971 |
| lookup indexed_groups（保守量） | 136,833 | 140,186 |
| indexed_walks | 139,368 | 140,375 |
| walk indexed_groups（訪問数） | 9,460,318 | 2,357,531 |
| MayUseHyperpathPolicy inclusive CPU ms/tick | **1.476260** | **0.423400** |

旧入力の Legacy では lookup rebuilds / indexed_groups は0だった。
今回の Legacy にはこの既存意図の処理が含まれるので、上記 −0.481 ms/tick を
純粋な Legacy 定常状態に対する優位とは解釈しない。
Legacy を元入力から独立に定常化した比較は、今回の共通入力ペアの範囲には含めていない。

## 関数単位で見えた内訳

以下は主スレッドの StateGameLoop を含むスタックの sampled CPU ms/tick。
inclusive はスタックごとの関数の和集合で計算し、親と子を合算しない。

| 関数 / 直下の枝 | Legacy inclusive | Hyperpath inclusive | 差 |
| --- | ---: | ---: | ---: |
| PassengerServiceDatabase::Tick | 1.174075 | **3.022997** | **+1.848922** |
| RunPassengerWaitingEvents | 2.067709 | 2.257914 | +0.190205 |
| CallVehicleTicks | 13.054205 | 11.223985 | −1.830220 |
| RunTileLoop | 10.359693 | 9.703222 | −0.656471 |
| LoadUnloadVehicle（CallVehicleTicks 配下） | 3.796285 | 2.551918 | −1.244366 |

Hyperpath の追加費用が最も大きい枝はテーブル構築の `PassengerServiceDatabase::Tick`。
その内側の `TickPrecomputed` は Legacy 1.154500 / Hyperpath 3.010733 ms/tickだった。

| 関数 | Legacy exclusive | Hyperpath exclusive | Hyperpath inclusive |
| --- | ---: | ---: | ---: |
| PassengerPrecomputedTable::StepHyperpath | 0 | **1.328931** | **2.954580** |
| PassengerPrecomputedTable::SetSearchLabel | 0.084060 | **0.530400** | 0.530400 |
| PassengerPrecomputedTable::MarkBoardingCandidate | 0 | 0.149193 | 0.150167 |
| PassengerJourneyLoadingContext::MayUseHyperpathPolicy | 0.189474 | 0.063490 | 0.423400 |
| StationCargoList::RefreshHyperpathLookup | 0.015149 | 0.019534 | 0.166526 |

次に詳しく調べる候補は **StepHyperpath とその内側のラベル・キュー操作**。
これは今回の Hyperpath の絶対費用とサンプルに基づく調査候補で、追加のコード変更は行っていない。
Legacy 側の既存意図、RunTileLoop 等の差もあるため、総差を一つの最適化へ帰属させていない。

[Legacy全関数](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/legacy-functions.csv)、
[Hyperpath全関数](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/hyperpath-functions.csv)、
[全関数の inclusive / exclusive 差](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/function-diff.csv)、
[StateGameLoop 直下の枝](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/branch-diff.csv)、
[差の上位各20件](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/top-function-differences.csv)。

## 証跡の検証と保持

最初の Legacy 記録中に別ワークツリーのビルドが始まり、競合検出で無効となった。
その時間を採用せず、ビルド終了後に両モードを取り直した。
受理した二実行は競合0、exit 0、正確に2,048 tick、支払い valid。
Legacy の終了 canonical は無効試行とも24/24一致した。

WPR の Lost Events / Lost Buffers は両方0、製品関数の未解決表記0。
Function / Stack 表の総 Weight と件数は all / main / simulation_stack の全範囲で保存則が成立。
主スレッドのスタック不明は Legacy 730/64,784、Hyperpath 671/58,693。
WPA Invalid Event 警告は各1件あり、87.115844秒 / 80.993506秒はいずれも
集計終了65.751秒 / 59.667秒より後だった。両 export は exit 0。
原文 CSV と gzip 展開後の SHA はすべて一致した。

同一 EXE の前回の728テスト / 9,930,179 assertions 成功を引き継ぎ、今回は再ビルドしていない。
セーブ・アセット・EXE/PDB・生 ETL（Legacy約2.48 GB / Hyperpath約4.52 GB）は
ignored の `build-passenger-steady-8462e6cd/` に保持する。

[再現手順](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/README.md)、
[WPR集計](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/summary.json)、
[バイナリ・PDBの由来](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/build-identity.json)、
[検証結果](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/checks/validation.json)、
[証跡SHA-256](measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/evidence-sha256.json)。
