# 路線ベースTimed探索の検証記録

ベース `c2c4d1898c3a38a091ff224c5be660ede72238f7`、ブランチ `feat/passenger-timed-round-based`。

正式測定は継続状態frontier索引版。Release、MSVC、assert有効、LTO無効、`OPTION_PASSENGER_BENCHMARK=ON`。65,536 Work、8,192 Labels、8MiB。全条件で同じセーブ・資産・言語・バイナリを使い、旧→新→新→旧の順で8,192 ticksを実行する。入力SHA-256は `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`。StateTicksは `3176545487` → `3176553679`。

CPU予約、90秒の静穏確認、5秒間隔の競合検出を使用する。CPUの計測はWindowsスレッドCPUの合計とプロセスCPUを分ける。前者はOD検索、後者はロード・保存・workerを含む。simulation p95/p99は1ms幅ヒストグラムの上限である。

`quality-console.txt` はStateTicks `3176545999` における最初の256個の異なる待機要求。ODだけでなくreadyと直前Service除外を合わせた。各方式のDBを独立復元し、同じライブ状態を参照する。大予算の旧方式は1,048,576 Work、65,536 Labels、128MiB。延期・連続同Service乗換・Dispatch待機を省く参照は認証済み比較から除く。駅ID順の標本であり、全国ODの無作為標本ではない。

`multiplayer-result.json` と各consoleは実マップ、ローカル127.0.0.1:17492、初回・途中参加の2クライアント。nullドライバーのクライアントにはstdin consoleがないため、既存 `on_client.scr` と月初のlocal scriptで共通チェックポイントを記録した。3プロセスのStateTicks・設定ONと継続ACK、同期失敗の有無を確認する。

途中の試行は正式性能値に混ぜない。GUI subsystemでconsole出力を取得できなかった試行、クライアントのgame_start/stdinを前提にした検証、PowerShell 5でGet-FileHashを解決できず記録に失敗した旧方式ランは除外した。初期品質標本は改善の診断にのみ使う。既存JP+ Multiple Unitsの容量警告は両方式のstderrとmanifestに保持する。

再実行は `utils/passenger_timed_rounds_verify.ps1`、`utils/passenger_timed_rounds_benchmark.py`、`Exercise-Multiplayer.py --rounds` を使用する。品質比較は `passenger_timed_compare 256`。入力セーブ・NewGRF・実行ファイルはリポジトリに含めない。

正式4ランは完了し、新旧それぞれの最終Saveが2反復で一致した。結果と原因の診断は [実装・結果報告](../../passenger-timed-rounds-2026-10-14.md) を参照。平均simulationは18.64%退行、検索CPU/ODは45.03%増。Label試行80.27%減・Timed配送15.29%増という改善はあるが、総配送は0.0815%減で目標未達。

保存した集計を再生成するには以下を実行する。

```text
python Summarize-Comparison.py performance-manifest.json performance-summary.json
python Summarize-Quality.py quality-console.txt quality-summary.json
```

quality-summaryのsource欄は入力パスに依存し、それ以外の集計値は同じconsoleから再現できる。raw stderrはignoredの `build-rounds/comparison-partition/<run>/` に保持し、GitにはmanifestのSHA-256とunique warning linesを保存する。JP+ Multiple Unitsの容量警告は旧新とも存在する。警告を理由に成功を無条件で仮定せず、反復Save一致と実マップの共通checkpoint/ACKを別に確認した。

Git内のserver consoleと回帰consoleでは行末空白・末尾空行を整えた。元のrawファイルはbuild-roundsに保持し、数値と集計結果は変更していない。
