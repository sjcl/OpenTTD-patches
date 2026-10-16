# 測定記録の読み方

基点は `902997e6314e018a08c4b92ec0083b040790005f`。製品の最終実装は
`3429f283b2`、採用固定・pushのcheckpointは `667e154816`。
試行ごとの実行ファイルSHA-256、source manifest、引数はJSONに記録する。
ゲームのセーブ、NewGRF等の素材、実行ファイルはコミットしない。

## 最終構成の根拠

- `final-experiments`：カウンターなしの基点・採用scalar・SIMD・世代番号比較。
  準備と全目的地の合計133.6→60.3秒を採用の主たる時間測定とする。
- `final-integrated-correctness`：最終scalarの全Label、全OD・4sample・4除外条件の
  Plan、共通区間キーを旧クラスと比較。1,255,161,055 assertions成功。
  ビルドとの重複があるためこの試行の壁時計を性能値として使わない。
- `final-counted-source.json` / `experiment-source.json`：そのコピーTUのハッシュ。
- `ctest-final-integrated.log`：計測TUを外した製品の693 CTest。
- `final-synthetic.log` / `final-slices-*.log`：合成・微小Work予算の独立比較。
- `experiments2-*-checks.log`：heap・SIMD比較とscalar強制fallbackの検査。

## 段階的な測定

- `integrated-heap-matrix-corrected`：同じRangeグラフの5種類のheap。
  初期の4-ary試験コードの不具合を修正し、順序テストと完全一致を通した試行。
- `integrated-counted-matrix`：同じ5方式等の操作数・1/1024のheap時計標本。
  初版計測のrelaxation値はcold materializationも含んでいた。
  `recorded_relaxations`に元値を保存し、別計数のmaterializationを引いて補正した。
  `raw.log`は元のまま。最終独立検査ではツールを修正し、補正なしに同じ数を確認した。
- `integrated-stages`：hot/cold、Range、ブロック初期化、ノードバッチ、初期SIMD、
  Range後の縮約を段階比較。最終版のsource優先度のループ外配置以前の構成。
- `heap-matrix-real`：CSR/Range以前のSoAグラフでの旧比較。
- `final-timing-real` / `final-correctness-real`：歴史的な名前。
  **Range導入前**の時間・全Plan比較であり、今回の最終構成とは異なる。
- `real-fixture/source.json`：固定したJPSDと駅座標の入力ハッシュ。

## 通常tick・MP・保存

この資料ディレクトリはGitの改行変換を無効にし、測定原文のbyteを保持する。
生ログに含まれる末尾空白もそのまま保存する。
`artifact-sha256.json`は、そのmanifest自身を除く資料ファイルのSHA-256を記録する。

通常tickは各ケースの`process.json`に実際のStateTicks、全tick標本、OS peak、
他ビルドの検出、NewGRF警告・error pauseの測定用解除回数を保存する。
準備・ロード・保存を含むプロセス時間と、simulationの平均/p95/p99を混同しない。
`phase_contamination`は準備・simulation・保存後の競合を区別し、計測期間の競合は拒否する。
全プロセス時間の有効性とsimulation標本の有効性を別々に記録する。
`runtime-matched-*`は通常版・静的CRT・assert ON・LTO OFFを揃えた比較。
`matched-build`に実際のVS定義、バイナリSHA、DLL依存とビルドログを保存する。
`runtime-core-control`は全プロセス、`runtime-main-core`は主スレッドだけをCPU 0へ固定した補助比較。
前者はworkerも直列化する条件で、通常tickの採否判定には使わない。
後者の`affinity-audit.json`はOSから読んだ実際のthread maskとprocess mask。
`thread-cpu-window.json`は開始logのcloseからtick出力までを50ms間隔で外部観測した値で、
生のtick標本そのもののCPU時間ではない。境界の観測誤差を含む補助情報として扱う。
`runtime-control-method`に両runner、観測手順、旧製品ソース14件のコミット照合を残す。
記録したrunnerは`build-msvc/run-matched-validation.py`として実行したsnapshotであり、
再実行にはその場所へコピーし、記録された固定入力packageと旧バイナリを用意する。
`runtime/`の初期試行には、旧通常版/静的CRTと新dedicated版/DLL CRTの構成差がある。
これらは最終採用の性能比較に使わず、構成差の調査記録として残す。
`runtime-discarded`の試行は、日時行の読取失敗または外部ビルドとの重複がある。
要求tick数の検査・負荷の検査に通過していないため性能比較には使わない。
このセーブには元からNewGRF警告があり、警告のないmapの検証ではない。

MPは`passenger_precomputed_join.py`による同一実行ファイル同士のloopback接続。
初期準備、参加時再生、公開/保持行とWork、canonical packet/payment/ownership/loading、
配送数を比較する。ハーネスの観察時間を長くしても製品の参加期限は変更しない。
ネットワークlogを抽出した場合は`network.log`へ保存し、GRF等を含む元のstderr全体は
ローカルのbuild出力に保持する。`result.json`に元のGRF警告行数も記録する。

診断readbackは保存直後と再読込直後のcanonical状態・イベント索引を照合する。
再生を始める前のため、derivedな行数・Workは比較対象から除く。
構築cursorの完全再生はネイティブの旧版/v10/v11回帰と実MPの再生で別に検証する。

`result.json`の時間単位は`*_ns`がns、プロセス経過時間が秒。
`details.graph_layout`は個別辺数、Range数、論理辺数、CSR作成ns。
`details.phases`はSection構築ns、1目的地の最小/中央値/p95/最大ns。
`details.memory`は完成Label容量、heap容量、旧Label作業領域、SoA作業領域のbyte。
`array_bytes`は配列容量の合計でallocator overheadを含まない。
比較プロセスのOS peakは同時に保持する2表を含み、1製品表の常駐量ではない。

## 再現

MSVC x64 Release、assert ON、LTO OFF。`passenger_table_build_prepare.py`で
別名の旧クラスと候補をbuild内のコピーTUへ生成してから`openttd_test`をビルドする。
`--timing-only`は辺単位カウンターを除去する。`--matrix`は5 heap、`--simd`はSIMD版、
`--stages`は段階比較を生成する。通常の製品へ実験コードをリンクしない。

```powershell
python -B utils/passenger_table_build_prepare.py build-msvc --candidate 667e154816 --heap product --timing-only --simd --matrix
cmake -S . -B build-msvc "-DCMAKE_PROJECT_INCLUDE=$PWD/build-msvc/table-build/benchmark.cmake"
cmake --build build-msvc --config Release --target openttd_test --parallel 12
python -B utils/passenger_table_build_run.py build-msvc/Release/openttd_test.exe build-msvc/repeat --fixture build-msvc/real-fixture --experiments --skip-plan
cmake -S . -B build-msvc -DCMAKE_PROJECT_INCLUDE=
```

JSONの引数・入力ハッシュと同じ固定snapshotを使う。CPU時間の比較中には他のビルドや
ゲームを動かさない。全Plan比較は`--skip-plan`を付けずに行う。

ホストを共有するチャット間では、`%TEMP%/openttd-passenger-routing-measurement.lock`を
排他的作成で取得し、所有PIDと工程をJSONへ記録する。時間で枠を予約せず、実際の取得・解放で交代する。
所有プロセスだけがロックを再利用でき、待機側は他の所有者のロックを削除したりプロセスを止めたりしない。
この記録の`runtime-control-method/run-*.py`類は、ビルド・計測・通常ビルドへの復元まで
同じ所有者で保持した実行手順である。個別の終了状態とソースhashも合わせて確認する。

`reference-build/`は基点902997e631を同じ通常版・静的CRT環境で新たにビルドした記録、
`compiled-code-comparison/`はMSVC COFFの再配置部分を除いた関数byte列の比較である。
後者は時間の測定ではなく、通常tickの不悪化や差の原因を証明するものではない。

`runtime-same-image/`は同一実行ファイルの新→旧→旧→新の各2,048tick。
旧モードは既存の再生エンジンを使い、計測用DBコピーだけで即時移行を抑える。
`same-image-control/canonical.json`は全保存項目の照合、`counter-differences-a.json`は
通常動作カウンターの差、`runtime-same-image-components.json`は既存`fps`診断の末尾窓である。
最初の補助診断のコマンド誤記と、残りの終了スクリプト訂正の前後hashも保存した。
訂正はtick開始前で、元の主標本とconsole記録は変更していない。

`host-observed-control/self-probe-preparation.json`はWindowsの読み取りAPIの動作確認で、
ゲームの性能試行ではない。任意指定の`--observe-host`はOSが提供するカウンターをそのまま記録し、
raw tickの補正や電源・スケジューラ設定の変更はしない。スレッド一覧取得は準備中だけに限定する。

`background-ready-control/`は基点902と現在の製品をそれぞれ実際にビルドした比較で、
Release設定の一致、1,866基点ソースの照合数、バイナリhash、計測用ソース、通常構成への復元を記録する。
`runtime-background-ready/`は旧A→新A→新B→旧Bの通常時各2,048tick。
`--wait-background-ready`で完成表のwarm後にロード時LinkGraphJob全件の完了を待ち、
前後StateTicksが同じことを検証する。結果を早期に取り込まず、製品や参加期限には待機を加えない。
旧新版の構築所要時間が異なることで、未完了ジョブとの競合時間が偏る問題を除くための測定準備である。

`runtime-background-ready-retained/`は同じ保存済みバイナリを再利用し、warmとジョブ待ちの後に
transfer penaltyを17へ変更して旧表を保持した更新を測る。各版の全canonical項目を、
以前の同じ版の終了状態と照合する。
`runtime-background-ready-analysis.json`は各試行と各版4,096tickの統計、
`runtime-control-method/summarize-background-ready.py`はその集計手順である。
全raw tickを保存し、外部CPUカウンターで時間を補正したり標本を除外したりしない。
外部観測区間の非主スレッドCPU時間は全補助threadを含み、正確なStateGameLoop境界ではない。
