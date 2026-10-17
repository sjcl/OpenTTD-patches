# Legacy / Hyperpath の WPR CPU サンプリング

同じセーブから Legacy と Hyperpath（フィルタ ON）を各2,048 tick 実行し、
WPR の CPU プロファイルを採取した。
**StateGameLoop 内の Hyperpath 増分は +2.518 CPU ms/tick。
中心は車両処理 +1.754 と待機旅客処理 +0.617 ms/tickだった。**
LocalPolicy の inclusive は0.068 ms/tickで、この記録では増分の主因ではない。

ただし記録区間全体では、Legacy 側に表準備の追加コストがある。
それを含む主スレッド全体の差は +0.066 CPU ms/tickだった。
「simulation tick の差」と「記録区間全体の CPU 差」を分けて読む必要がある。
製品の性能コードは変更していない。

## 関数別 CSV

- [Legacy の関数別 inclusive / exclusive](measurements/passenger-hyperpath-wpr-2026-10-17/legacy-functions.csv)
- [Hyperpath の関数別 inclusive / exclusive](measurements/passenger-hyperpath-wpr-2026-10-17/hyperpath-functions.csv)
- [関数ごとの差分](measurements/passenger-hyperpath-wpr-2026-10-17/function-diff.csv)
- [StateGameLoop 直下の処理別差分](measurements/passenger-hyperpath-wpr-2026-10-17/branch-diff.csv)
- [呼び出し先別の内訳](measurements/passenger-hyperpath-wpr-2026-10-17/caller-breakdown.csv)
- [ヒープ処理の呼び出し元別内訳](measurements/passenger-hyperpath-wpr-2026-10-17/allocation-callers.csv)

`scope=main` は主スレッド全体、`all` はゲームの全スレッド。
`simulation_stack` は主スレッドのうち StateGameLoop がスタックにあるサンプル。
inclusive は各スタックでその関数に1回だけ加算し、再帰の重複を除く。
main/all の exclusive は WPA の命令ポインタの Function 列から直接集計した。
simulation_stack の exclusive は該当スタックの leaf から求めた。
各モードのサブフォルダには、WPAExporter の原文 CSV を gzip で保存してある。
CSV のサンプル数は呼び出し回数ではない。

## tick と記録区間の範囲

単位は ms/tick。差は Hyperpath − Legacy。
wall タイマーの行以外は WPA のサンプル Weight を2,048で割った CPU 時間。

| 範囲 | Legacy | Hyperpath | 差 |
| --- | ---: | ---: | ---: |
| 製品 simulation wall タイマー | 26.060 | 28.434 | +2.373 |
| StateGameLoop の sampled CPU | 24.323 | 26.841 | +2.518 |
| 主スレッド全体の sampled CPU | 28.486 | 28.552 | +0.066 |
| ゲーム全スレッドの sampled CPU | 28.677 | 28.745 | +0.068 |
| PreparePrecomputed inclusive（上の主スレッドに含まれる） | 2.201 | 0.011 | −2.190 |

初期の warm とロード済みジョブの待機は停止中に済ませたが、
記録区間中に製品の `GameLoop → PreparePrecomputed → PrepareGraph → BuildSections`
も実行された。これは `StateGameLoop` の外にある。
Legacy の driver loop は2,323回、Hyperpath は2,050回で、
どちらも実際に進んだ StateTicks は2,048。
したがって追加の GameLoop 呼び出しも含む区間全体では、上の表のように相殺がある。
この結果を「Hyperpath がゲーム全体で2.5 ms/tick余計に CPU を使った」とは解釈しない。

## simulation 内の増分

StateGameLoop 直下で分割した次の行同士は重複しない。

| 直下の処理 | Legacy CPU ms/tick | Hyperpath | 差 |
| --- | ---: | ---: | ---: |
| CallVehicleTicks | 12.526 | 14.280 | +1.754 |
| RunPassengerWaitingEvents | 1.195 | 1.812 | +0.617 |
| RunTileLoop | 9.890 | 9.988 | +0.097 |
| PassengerServiceDatabase::Tick | 0.124 | 0.146 | +0.022 |

以下は主スレッドの関数別結果の抜粋。
親子の inclusive は重複するため、行を足し合わせない。
VisitBoardingCandidates は CargoLoad 用インスタンスを示す。

| 関数 | Legacy inclusive | Hyperpath inclusive | inclusive 差 | exclusive 差 |
| --- | ---: | ---: | ---: | ---: |
| LoadUnloadVehicle | 1.216 | 2.314 | +1.098 | +0.002 |
| VisitBoardingCandidates (CargoLoad) | 0.742 | 1.644 | +0.902 | +0.336 |
| MayUseHyperpathPolicy | 0.000 | 0.411 | +0.411 | +0.077 |
| RecordCapacityMiss | 0.000 | 0.187 | +0.187 | 0.000 |
| LocalPolicy | 0.000 | 0.068 | +0.068 | 0.000 |

LoadUnloadVehicle 直下では StationCargoList::Load が +0.900、
RecordCapacityMiss が +0.187 ms/tick。
RunPassengerWaitingEvents の増分は、ほぼ RoutePassengerJourneys の +0.615 にある。
別に YapfTrainChooseTrack が +0.335 ms/tick増えており、
ヒープ呼び出し元の内訳でも YAPF ノード破棄における RtlFreeHeap が +0.252だった。
ヒープ全体の増分をすべて Hyperpath のメモ化コンテナに帰属させる根拠はない。

次に調べる第一候補は **積み込み候補の処理と membership メモ化**
（VisitBoardingCandidates / MayUseHyperpathPolicy）、
第二候補は **RoutePassengerJourneys の待機インデックス更新**。
これは上の呼び出し経路とサンプルからの判断で、今回コード変更はしていない。
LocalPolicy の代用を最初に行うだけの時間的な根拠は、この記録にはない。
関数単位のサンプルだけでは VisitBoardingCandidates の self を
走査・キー統合・候補キューに分解できないため、self の全量を走査コストとはみなさない。

製品の走査タイマーは ON の144,211走査で2,025,977 µs、
約0.989 ms/tickだった。これは走査に含まれる下位処理とも重なり、
上の inclusive に足す値ではない。

## 条件と検証

取得時 HEAD は `cdd7513528c96565677c92f68d3d0cb8caa98294`、
製品 src は `594bbecad3` と同じツリー
`cb3e0ee59208f17636cbe8a48cd8a7f4660121de`。
MSVC x64 Release、assert ON、LTO OFF、静的 CRT、`/Zi /Zo` と対応 PDB を使用。
Ryzen 9 9950X、16コア/32論理CPU、Windows build 26200。
主スレッドだけ affinity mask 1、ワーカーは固定しない。

前回と同じ凍結セーブ、入力 SHA-256
`306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
両方とも StateTicks `3181119581..3181121629`。
途中の意図的なコスト更新は入れていない。
Legacy は precomputed ON / hyperpath OFF、Hyperpath は両方 ON / filter ON。
開始・終了マーカーで記録区間を指定し、終了集計とセーブは区間外。

両方とも正常終了、支払い状態は valid、監視対象の別ゲーム・ビルド・テストの競合検出は0。
MSVC の既存 **727テスト / 9,899,874 assertions** も成功した。
Legacy と Hyperpath は輸送判断が同一でないため、終了時の Packet 数等も異なる。
この比較は同じ入力からのアルゴリズム比較であり、同一クエリ列だけの比較ではない。

Lost Events / Lost Buffers は両方0。
WPA の原文スタックと exclusive 表の総サンプル数・Weight は、
両モード・全スレッド/主スレッドの各範囲で完全一致した。
ゲーム本体の Function 列のサンプルは Legacy 47,152、Hyperpath 46,207で、
その中に未解決関数名はなかった。
スタック自体が n/a の主スレッドサンプルは782/58,321と670/58,461。
これらは inclusive の特定関数へ推測で配分しない。

WPA は両 ETL の停止処理中に各1件 Invalid Event を警告した。
対象時刻は82.111秒/81.344秒で、集計した終了マーカー59.668秒/59.797秒より後。
原文警告はログと summary に残した。未解決の一部システムドライバも生 CSV に残す。
1ペアのサンプリングであり、微小差の再現性までは主張しない。

[集計 JSON](measurements/passenger-hyperpath-wpr-2026-10-17/summary.json)、
[再現手順](measurements/passenger-hyperpath-wpr-2026-10-17/README.md)、
[ビルド・PDB・スクリプト識別](measurements/passenger-hyperpath-wpr-2026-10-17/build-identity.json)、
[証跡 SHA-256](measurements/passenger-hyperpath-wpr-2026-10-17/evidence-sha256.json)。
生 ETL（約2.19 / 2.27 GB）と PDB は ignored の `build-wpr-profile/` に保持し、
ゲームプロセスだけに絞った CSV と再現資料を Git に含めた。
