# Hyperpath 8,192 tick 後の凍結入力と新しい WPR 基準

実施日: 2026-10-17〜18 JST。製品 commit: `8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4`。
製品ソース、計測用バイナリと PDB は前回から変更していない。
入力が変わるので、旧入力の WPR との補正済み差は計算しない。
この証跡の Legacy / Hyperpath は新しい入力に対する基準計測である。

## 凍結した入力

ローカル package:

```text
C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-passenger-steady-8462e6cd/frozen-8192
```

`input.sav` SHA-256:

```text
275544f1fd7bb16920ebd325c3a916ee06f164fcbe53abf0c1bb6409e9802f9f
```

元の凍結 package は以下に保持する。

```text
C:/Users/bebe/.codex/worktrees/5a69/OpenTTD-patches/build-hyperpath-cpu/frozen
```

元入力の SHA-256 は `306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
Hyperpath / index で 3181119581 → 3181127773 の 8,192 StateTicks を実行して保存した。
`frozen/package.json` はセーブ、設定、全アセットの SHA-256 を記録する。
元入力、新入力、アセット、生 ETL、PDB はローカルの ignored directory に保持する。
Git には原文 CSV の gzip、診断、canonical、ハッシュ、実行スクリプトを保存する。

## 再現手順

以下の `$repo` はリポジトリの絶対パス、`$work` は未使用の出力パスとする。
各 replay 出力 directory と freeze package は未作成である必要がある。
主スレッドのみ affinity mask 1、NewGRF error pause の解除と全警告はログに保存する。
他の OpenTTD・ビルド・テストとの競合は共通 lease と runner の観測で検出する。

```powershell
$repo = 'C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches'
$build = "$repo/build-wpr-8462e6cd"
$work = "$repo/build-passenger-steady-reproduce"
$old = 'C:/Users/bebe/.codex/worktrees/5a69/OpenTTD-patches/build-hyperpath-cpu/frozen'

# 元入力を Hyperpath に移行し、8,192 tick 実行する。
$env:OPENTTD_PASSENGER_CHECKPOINT_TICK = '4096'
python -B utils/passenger_precomputed_real_map.py run "$build/Release/openttd.exe" $old "$work/warmup" --ticks 8192 --timeout 2400 --measurement-wait 120 --precomputed --hyperpath --hyperpath-filter-mode index --cold-precomputed --warm-precomputed --wait-background-ready --main-thread-affinity-mask 1 --lang "$build/lang"
Remove-Item Env:OPENTTD_PASSENGER_CHECKPOINT_TICK
python -B utils/passenger_precomputed_real_map.py freeze "$work/warmup/result.sav" $old $build "$work/frozen-8192"

# 設定と構築カーソルを保ったまま、tick を進めず再ロードする。
python -B utils/passenger_precomputed_real_map.py run "$build/Release/openttd.exe" "$work/frozen-8192" "$work/reload" --ticks 0 --timeout 2400 --measurement-wait 120 --resume --main-thread-affinity-mask 1 --lang "$build/lang"
```

`warmup/save-integrity.json` と `reload/save-integrity.json` を Python の整数精度で比較する。
canonical 24項目がすべて一致することが凍結・再ロードの検証条件。

WPR の準備では、対応する EXE/PDB を `$work/Release/`、
`cpu-sampled.wpaProfile` を `$work/` に置く。既存 Windows / 製品 symcache を再利用した。
`utils/passenger_wpr_record.ps1 -Build $work` を管理者として起動する。
ゲームと runner は通常権限で実行する。recorder は gate を待ち、各 replay の timed ticks を
`wpr -start CPU` と `wpr -stop <mode>/cpu.etl` で記録する。

```powershell
python -B utils/passenger_precomputed_real_map.py run "$build/Release/openttd.exe" "$work/frozen-8192" "$work/legacy" --ticks 2048 --timeout 2400 --measurement-wait 2400 --resume --precomputed --no-hyperpath --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1 --lang "$build/lang" --wpr-profile
python -B utils/passenger_precomputed_real_map.py run "$build/Release/openttd.exe" "$work/frozen-8192" "$work/hyperpath" --ticks 2048 --timeout 2400 --measurement-wait 2400 --resume --precomputed --hyperpath --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1 --lang "$build/lang" --wpr-profile
```

WPR replay は `--resume` を付け、`--cold-precomputed` を付けない。
両モードとも既存の paused warm と background-ready 待ちを行い、計測直前に診断を reset する。
Legacy はこの Hyperpath 入力からモードを false へ切り替えるので、モード変更の callback と
テーブル準備は timed ticks の外側で実行する。Hyperpath は保存済みの true を維持する。
両実行とも 3181127773 → 3181129821。モード間の終了状態の一致を要求しない。
今後の変更はこの同じ新入力・同じ手順から実行し、同じモードの canonical と比較する。

Legacy の設定を false にしても、セーブ内の既存 HyperpathIntent は計測中に処理される。
今回は Legacy でも lookup `rebuilds=2984; indexed_groups=136833`（旧入力の Legacy は両方0）。
したがってこのペアは同じ Hyperpath 入力から設定を切り替えた比較であり、
Legacy も独立に十分走らせた定常状態の比較とは区別する。
`PassengerJourneyLoadingContext::MayUseHyperpathPolicy` は packet の intent kind に応じて判定する
（製品 commit の `src/passenger_journey_active.cpp`）。

```powershell
python -B utils/passenger_wpr_export.py $work '<Windows Performance Toolkit>' --output-name wpa --local-symbols-only
python -B utils/passenger_wpr_summarize.py $work '<evidence directory>' --export-name wpa
```

`legacy-functions.csv` / `hyperpath-functions.csv` は全関数の inclusive / exclusive、
`function-diff.csv` は Hyperpath − Legacy、`branch-diff.csv` は StateGameLoop 直下の内訳。
`top-function-differences.csv` は差の上位各20件。
inclusive はスタックごとの関数の和集合であり、親と子の値は合算しない。
全スレッド / 主スレッドの exclusive は WPA の Function 列、
`simulation_stack` の exclusive は StateGameLoop を含むスタックの末端に帰属する。

`input-window-context.csv` の旧入力値は別 tick 区間の参考値。
`passenger_wpr_control.py <旧入力の証跡> <この証跡>` による補正には使えない。
今後この入力を使った同条件の計測同士で補正する。

最初の Legacy WPR は別のビルドと重なり、runner が無効と判定した。
この記録の時間は使わず、ビルド終了後に `checks/retry_pair.py` で両モードを取り直した。
無効な実行の診断は `checks/rejected-legacy/`、生 ETL は local の `legacy-contaminated/` に保持する。
再試行では idle host の待機上限 `--measurement-wait` を2,400秒に延長した。
timed ticks の条件は同じで、受理した実行は競合0が必須。

`checks/measure_steady.py` は凍結・再ロード・最初の WPR replay の実行コード、
`checks/analyze_steady.py` は書き出しと集計、`checks/validate_steady.py` は照合コード。
バイナリ・PDB・計測用 TU の完全な由来は前回の `build-identity.json` に連結している。
