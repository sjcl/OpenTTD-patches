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

通常tickは各ケースの`process.json`に実際のStateTicks、全tick標本、OS peak、
他ビルドの検出、NewGRF警告・error pauseの測定用解除回数を保存する。
準備・ロード・保存を含むプロセス時間と、simulationの平均/p95/p99を混同しない。
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
