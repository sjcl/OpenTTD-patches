# 非同期旅客事前計算表の測定記録

詳細は [性能・決定性レポート](../../passenger-precomputed-async-tick-2026-10-18.md)。
`benchmark-summary.json` は各条件3回の中央値、`benchmarks/*/result.json` と `.log` は150試行の生値。
`native-tests.log` は参照逐次エンジンを含む741テストの全出力。
`multiplayer/` はコピーした計測用TUで実行する実ネットワーク試験の結果。
ゲームのバイナリ、アセット、セーブファイル、ユーザーの設定・認証情報は収録しない。

## 再現

通常のRelease＋assert有効のビルドを用意する。Windowsでの測定はMSVC 19.44.35207 x64。
参照比較には `python utils/passenger_parallel_reference_prepare.py BUILD` を実行し、生成された`parallel-reference/reference.cmake`を`CMAKE_PROJECT_INCLUDE`に指定して`openttd_test`をビルドする。
基準エンジンは同スクリプト内で`093218f882`に固定している。

```powershell
openttd_test.exe '~[.]' --reporter compact
python utils/passenger_parallel_benchmark.py BUILD/openttd_test.exe OUT/initial --scenario initial --repeat 3
python utils/passenger_parallel_benchmark.py BUILD/openttd_test.exe OUT/natural --scenario natural --repeat 3
python utils/passenger_parallel_benchmark.py BUILD/openttd_test.exe OUT/ahead --scenario ahead --workers 0 8 --repeat 3
python utils/passenger_parallel_benchmark.py BUILD/openttd_test.exe OUT/late --scenario late --workers 0 8 --repeat 3
```

各行に`--uneven`を加えれば958駅の不均一入力になる。naturalの不均一入力はWorker 0、8で測定した。
initialとnaturalはWorker 0、1、2、4、8。LegacyとHyperpathはスクリプトが両方実行する。
各試行でWork、tick数、公開・保持Row数の進捗digest、全Row、DB保存digestを比較し、不一致は失敗にする。

MPは `utils/passenger_async_probe_prepare.py SOURCE PROBE REVISION` で製品からコピーした計測用TUを作り、`PROBE/probe.cmake`を指定してゲームだけをビルドする。
このツールは既存の`SOURCE/build-parallel/parallel-reference/reference.cmake`を参照する。
プローブ用の変更を製品ソースへ適用する必要はない。

```powershell
python utils/passenger_async_join_matrix.py BUILD/openttd.exe ASSET_PACKAGE regression/stationlist/test.sav OUT/multiplayer BUILD/lang --jobs 3 --ticks 2048 --port 17620
```

`ASSET_PACKAGE`はOpenTTDのbaseset等を保持するローカルフォルダ。ネットワークの既存タイムアウトは維持する。
試験後は製品用のCMake設定へ戻して再ビルドする。
measurement SHAとソースSHAはJSONに記録してあり、テスト用コピーのバイナリを製品と区別できる。
