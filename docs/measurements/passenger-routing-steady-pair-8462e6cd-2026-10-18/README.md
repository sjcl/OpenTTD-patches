# Legacy / Hyperpath をそれぞれ8,192 tick進めた定常入力の比較

実施日2026-10-18 JST。製品 commit `8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4`。
同じ EXE / PDB を再利用し、製品ソースは変更していない。

今回の Legacy は、Hyperpath の終了セーブから切り替えたものではなく、
元の入力から Legacy のまま8,192 tick進めたセーブである。
先ほどの Hyperpath も同じ元入力から同じ8,192 tickを進めている。
各方式の経路選択・輸送の履歴により二つの保存状態は異なるが、tick位置は一致する。

## ローカル入力と由来

元入力 package:

```text
C:/Users/bebe/.codex/worktrees/5a69/OpenTTD-patches/build-hyperpath-cpu/frozen
```

元入力 SHA-256:
`306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。

Legacy の凍結 package:

```text
C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-passenger-legacy-steady-8462e6cd/frozen-8192
```

SHA-256 と全アセットのハッシュは `frozen-legacy/package.json`、
準備実行の由来と保存時の24項目は `frozen-seed.json` を参照。
Legacy入力のSHA-256は
`b276c5de400651f646c129ab394bc97a686f38aa345b3a4a799e07c8a06289ca`。
Hyperpath入力のSHA-256は
`275544f1fd7bb16920ebd325c3a916ee06f164fcbe53abf0c1bb6409e9802f9f`。
Hyperpath の入力と原文WPRは先ほどの
`docs/measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/` のものを再利用する。
`hyperpath-reuse.json` が元commit・ETLのSHA・コピーした証跡ファイルのSHAを記録する。

準備区間は両モードとも3181119581 → 3181127773（8,192 StateTicks）、
WPR区間は3181127773 → 3181129821（2,048 StateTicks）。
セーブ、全アセット、対応 PDB、生 ETL はローカルの ignored directory に保持する。
Git にはCSVの gzip、集計、状態とハッシュを保存する。

## 再現

`$repo` はリポジトリ、`$work` は未使用の出力パスとする。
全プロファイル関連環境変数を外し、準備実行だけ読取専用の4,096 tick checkpointを有効にした。
元入力からの cold / paused warm、背景ジョブ完了待ち、主スレッド affinity mask 1 は両モードで共通。

```powershell
$repo = 'C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches'
$build = "$repo/build-wpr-8462e6cd"
$work = "$repo/build-passenger-legacy-steady-reproduce"
$old = 'C:/Users/bebe/.codex/worktrees/5a69/OpenTTD-patches/build-hyperpath-cpu/frozen'
$env:OPENTTD_PASSENGER_CHECKPOINT_TICK = '4096'
python -B utils/passenger_precomputed_real_map.py run "$build/Release/openttd.exe" $old "$work/warmup" --ticks 8192 --timeout 2400 --measurement-wait 2400 --precomputed --no-hyperpath --hyperpath-filter-mode index --cold-precomputed --warm-precomputed --wait-background-ready --main-thread-affinity-mask 1 --lang "$build/lang"
Remove-Item Env:OPENTTD_PASSENGER_CHECKPOINT_TICK
python -B utils/passenger_precomputed_real_map.py freeze "$work/warmup/result.sav" $old $build "$work/frozen-8192"
python -B utils/passenger_precomputed_real_map.py run "$build/Release/openttd.exe" "$work/frozen-8192" "$work/reload" --ticks 0 --timeout 2400 --measurement-wait 2400 --resume --main-thread-affinity-mask 1 --lang "$build/lang"
```

`warmup/save-integrity.json` と `reload/save-integrity.json` を Python の整数精度で比較する。
canonical全24項目が同じであることを確認してから追加WPRへ進む。

`utils/passenger_wpr_record.ps1 -Build $work -Mode legacy` を管理者として起動する。
今回追加した `-Mode` は単独記録を選択する引数で、既定値は従来どおり両モード。
ゲームと runner は通常権限で実行する。

```powershell
python -B utils/passenger_precomputed_real_map.py run "$build/Release/openttd.exe" "$work/frozen-8192" "$work/legacy" --ticks 2048 --timeout 2400 --measurement-wait 2400 --resume --precomputed --no-hyperpath --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1 --lang "$build/lang" --wpr-profile
python -B utils/passenger_wpr_export.py $work '<Windows Performance Toolkit>' --mode legacy --output-name wpa --local-symbols-only
```

`$work/Release/` に対応 EXE / PDB、`$work/` に `cpu-sampled.wpaProfile` を置き、
先ほどの Windows / 製品 symcache をコピーして再利用した。
再ロード・WPRでは `--resume` とし、coldリセットをせず、保存済みのLegacy設定を保つ。
WPRの計測開始前に paused warm と background-ready 待ちを済ませ、診断を reset する。

比較用に先ほどの Hyperpath の `wpa/`、scripts、診断・状態・export metadata を
`$work/hyperpath/` にバイト同一でコピーして、通常の両モード集計を実行した。
生ETLは元のローカル保存場所に保持し、重複コピーしていない。

```powershell
python -B utils/passenger_wpr_summarize.py $work '<evidence directory>' --export-name wpa
```

全関数の inclusive / exclusive は `legacy-functions.csv` / `hyperpath-functions.csv`、
Hyperpath − Legacy は `function-diff.csv`、StateGameLoop 直下の差は `branch-diff.csv`。
inclusive はスタックごとの関数の和集合なので、親と子を合算しない。
全スレッド / 主スレッドのexclusiveはWPAのFunction列、
`simulation_stack` のexclusiveはStateGameLoopを含むスタックの末端に帰属する。

両方式は異なる定常入力を使うので、相互のcanonical一致を要求しない。
`reload-canonical.csv` は各方式内の保存・再ロードの照合（計48項目）。
今後は同じ方式の同じ入力に対するWPRとcanonicalを比較する。
入力が変わる旧実行との `passenger_wpr_control.py` による補正は行わない。

実行コードは `checks/measure_legacy.py`、集計は `checks/analyze_legacy.py`、
検証は `checks/validate_legacy.py`、使用した共通ツールは `checks/tools/` に保存する。

最初の追加Legacy WPRは、別の `build-merge-validation` と重なったため無効とした。
次の再試行ではETL終了待ち中のビルドがrunnerに計測中と分類され、さらにETLから計測区間内の
短いCMake設定実行（44.520 ms）も見つかったため、この記録も無効とした。
他タスクの作業完了を確認してから `checks/retry_legacy.py` でWPRだけ取り直した。
再試行前はビルド・テスト・他のゲームが60秒間ないことを確認する。
無効な二記録の診断・canonical・ETLのSHAは `checks/rejected-legacy-1/` と `-2/` に保持する。
容量確保のため、この二つの無効ETL本体だけを削除した。終了canonicalの照合は
`legacy-retry-canonical.csv` に記録する。

runnerは、`wpr-finished.txt` ができた後の活動を `after_simulation` と分類するよう修正した。
このファイルは2,048 tick完了時のゲートで、`actual-ticks.json` はETL書き出し後に作られる。
修正前のrunnerも `checks/runner-before-wpr-phase-fix.py` に保存している。

追加した `utils/passenger_wpr_window.py` は、ETLのプロセス寿命とbegin/endマーカーを照合する。
WPA書き出し時にこの検査も実行する。CMakeの44.520 msの重なりを持つ実際の無効記録を拒否し、
既存Hyperpath記録と今回受理したLegacy記録は両方通過した。
検査結果は `checks/*-window-audit.json`。無関係なプロセスのコマンドラインは証跡へ含めていない。

最終試行では、tick終了後のETL書き出し中に別のOpenTTDが起動した。
raw `process.json` の `whole_process_timing_valid=false` と11件の `after_simulation` 観測は保持する。
`simulation_timing_valid=true` であり、独立したETL区間検査も重なり0なのでtick区間を採用した。
旧来の外側controllerは全プロセスの競合0も要求してassertしたが、runner自身は正常終了。
`checks/finalize_legacy.py` が2,048 tick、終了状態一致、全観測が計測後、ETL重なり0を照合し、
raw metadataを変更せず `checks/measurement-complete.json` に採用理由を記録する。
全プロセスの実行時間は性能値へ使わない。
