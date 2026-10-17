# Hyperpath boarding filter の実マップ比較

結果と解釈は [測定報告](../../passenger-hyperpath-filter-2026-10-17.md)。
測定対象は `544835fc59da1ec5e2d4e6793e9528107f2cb493`。
製品の `src` は変更していない。

`run-measurements.py BINARY PACKAGE LANG OUTPUT` は、同じセーブから
OFF → ON → ON → OFF の順で独立した4プロセスを実行する。
各回2,048 tick、中間の1,024 tickで乗換コスト20→17の表更新を起こす。
最初と最後の表構築を停止中に完了させ、その時間はtick標本から除外する。
全回で読み込み済みLinkGraphジョブの完了を待ち、主スレッドをCPU mask 1に固定する。
物理輸送追跡・乗車プロファイラは無効。CPU比較中はビルド・テストを実行しない。

実行には `utils/passenger_estimated_prepare.py SOURCE BUILD 544835fc59 --cpu`
で生成した計測版が必要。MSVC x64 Release、assert ON、LTO OFF、静的CRT。
`checks/real-map-instrumentation.json` にコンパイラ・リンクライブラリを記録する。
通常のCMake生成後に準備し、`passenger_hyperpath_build.py`でビルドする。
容量不足履歴も検査するため、生成された `save-integrity/cargopacket_sl.cpp` の
`const auto &r = cp.passenger_journey;` の前に次の観測のみを追加した。

```cpp
const auto &failure = cp.passenger_boarding_failure;
Mix(merge, failure.service, failure.count, failure.since, failure.incarnation);
```

`copied-tus/` は使用した計測用コピーの原文gzip。
製品の経路選択・イベント・論理Work・保存仕様を変更するフックは追加していない。
既存の計測用consoleはNewGRF error pauseを解除する。
警告と解除は各回の `stderr.txt.gz`、`on-1-console.txt.gz`、`process.json` に残す。

`summarize.py RUNS OUTPUT_JSON` は、完全なtick列、consoleのtick合計、
on/off切替→reset→unpauseの順、重複負荷なし、表準備完了、支払い検査、
中間/終了canonical全項目、実乗車/乗換/配送、容量不足の一致を検査する。
さらに `ON.checked + ON.skipped = OFF.checked` と admitted/walksの保存を確認する。
圧縮consoleを直接読めるため、この保存済み `runs/` から `summary.json` を再生成できる。

`archive-evidence.py BUILD PACKAGE` はセーブ・バイナリ・ゲーム資産を含めず証跡を複製し、
元packageの全asset hashと入力hashを照合する。`package.json` は元の資産manifestで、
実際の言語ファイルは各 `process.json` のhashを使う。
`source-identity.json` は製品ツリー、使用バイナリ、検査に使ったコピーのhash。
`evidence-sha256.json` はmanifest自身を除くこのディレクトリの保存byte hash。
`.gitattributes` は証跡の改行変換を防ぐ。
