# 67cbbf32 の WPR 計測と速度差の補正

計測対象は `67cbbf32a48bac840d49037b2f9a90e8a1ca3aba`。
製品 src ツリーは `a1efff1664b667558bb54260657a4c9108ce643c`。
チェックアウト `5e862d93f6e425876c905e22a5f4b84625a11915` は、これと同じ src に
`utils/passenger_wpr_control.py` だけを追加したもの。
比較元は [f7c44f0d の証跡](../passenger-hyperpath-wpr-f7c44f0d-2026-10-17/README.md)。

前回と同じ MSVC x64 Release、assert ON、LTO OFF、静的 CRT、
`/O2 /Ob2 /DNDEBUG /Zi /Zo`、リンク
`/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO` で独立した
`build-wpr-67cbbf32` にビルドした。元の src は編集していない。
計測用に複製した TU と識別情報は `copied-tus/`、`build-identity.json` に保存する。
全ネイティブテストを実行した後、共有計測ロックを守って Legacy → Hyperpath の順で走らせた。

```text
python -B utils/passenger_estimated_prepare.py SOURCE BUILD 67cbbf32 --cpu
python -B utils/passenger_wpr_prepare.py BUILD
python -B utils/passenger_hyperpath_build.py BUILD BUILD/build.log --target openttd openttd_test
BUILD/Release/openttd_test.exe --reporter compact
```

入力は前回と同じ凍結パッケージ `build-hyperpath-cpu/frozen`。
セーブの SHA-256 は `306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
実行の代表形は以下。`MODE` は legacy / hyperpath、`HYPERPATH_SWITCH` は順に
`--no-hyperpath` / `--hyperpath`。両方ともフィルタ ON、主スレッド affinity mask 1。
開始3181119581、終了3181121629の2,048 StateTicks、途中の意図的な設定変更はない。
コマンド・実際の設定・PID/TID・tick ごとのタイマーは各モードの `process.json` にある。

```text
python -B utils/passenger_precomputed_real_map.py run BUILD/Release/openttd.exe FROZEN BUILD/MODE --ticks 2048 --timeout 1800 --measurement-wait 120 --precomputed HYPERPATH_SWITCH --hyperpath-filter --cold-precomputed --warm-precomputed --wait-background-ready --main-thread-affinity-mask 1 --wpr-profile --lang BUILD/lang
```

WPR 専用ヘルパーだけを UAC で管理者起動し、ゲームは通常権限で走らせた。
`passenger_wpr_record.ps1 -Build BUILD` は、準備完了ゲートから
`wpr -start CPU`、開始マーカー、2,048 tick 後の終了マーカー、`wpr -stop MODE/cpu.etl`
を実行する。終了集計とセーブは記録区間外。ゲート・マーカーの記録を同梱する。

WPAExporter `11.7.395.48728` と前回と同じ `cpu-sampled.wpaProfile` を使い、
開始・終了マーカー間の CPU Usage (Sampled) を関数・スタック別に書き出した。
既存の Windows symcache をコピーし、ゲームの symcache は新しい対応 PDB から生成した。
ネットワークによるシンボル検索は行わない。各 export のコマンド、ETL/PDB/CSV の SHA と
終了コードは `MODE/export-command.json`、警告原文は `MODE/wpa-export.log`。

```text
python -B utils/passenger_wpr_export.py BUILD TOOLKIT --output-name wpa --local-symbols-only
python -B utils/passenger_wpr_summarize.py BUILD EVIDENCE --export-name wpa
python -B utils/passenger_wpr_compare.py docs/measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17 EVIDENCE
python -B utils/passenger_wpr_control.py docs/measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17 EVIDENCE
python -B EVIDENCE/checks/membership.py docs/measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17 EVIDENCE
```

`control.json` は最後のコマンドの標準出力をそのまま保存したもの。
既定の分類語・2,048 tick を変更していない。対象は `StateGameLoop` があるサンプル。
ntdll の関数名に `Heap` / `Block` / `LowFrag` / `Rtlp` を含むものを heap、
それ以外で既定の変更対象語を含むものを touched、
残りを untouched に排他的に分類する。untouched の新/旧比を `machine_factor` とし、
新しい時間をこの比で割って旧版との差を取る。これは共通の速度変動を仮定した推定値。
全関数が同じ割合で変動することを保証するものではなく、wall タイマーとも区別する。

`canonical-comparison.csv` は同じモード同士の終了24項目を Python の整数精度で比較する。
版情報が入る終了セーブのバイナリ SHA は canonical の判定に使わない。
`membership-before-after.csv` は `MayUseHyperpathPolicy` の新旧 inclusive / exclusive と
同じ machine factor による補正値。inclusive は1スタック内で関数を1回だけ数える。
子関数の inclusive を加算して二重計上しない。
exclusive は all / main では WPA の命令位置 Function 列、simulation_stack では
スタックの leaf を使う。今回、この関数には両者で2サンプルの差がある。

原文 WPA CSV は gzip で保存する。`*-functions.csv` は各モードの集計、
`function-diff.csv` は今回の Hyperpath − Legacy、`tick-branches-before-after.csv` は
同じモードの新旧比較である。YAPF 集計の範囲・方法は比較元と同じ。
CPU ms/tick は Weight / 2,048。サンプル数は呼び出し回数ではない。
生 ETL・対応 PDB・セーブ・資産は ignored の新旧 build ディレクトリに保持する。
