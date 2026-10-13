# Schedule-aware Journey 実装・検証（2026-10-13）

ベースは `7d914aa6f28293abb406becee2cec5fbb11e1d17`、ブランチは
`feat/passenger-journey-schedule-aware`。独立した
`passenger_journey_schedule.cpp` を追加し、周期DBの駅・OrderList索引を直接探索する。
Timedの `CompilePolicy()` 本体は変更していない。

経路選択だけでなく、系統Intent、別の共有実車両へのBinding、予約・分割・
Gradual Loading、実到着からの乗換制約、次車両への積載、最終ODへの配送へ接続した。
モデル・保存形式・制限は [実装仕様](passenger-journey-schedule.md) に記載する。

## 測定条件

同じ入力セーブ SHA-256
`14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`
から逐次実行。state ticks `3176545487→3176553487` の8,000tickを比較する。
月境界の途中標本と、全ループ終了後の最終標本を保存した。
月境界では進行中のtickの計数が未完了なので、最初の標本は1,526完了tickに対し
state tick差は1,527となる。両方式の同じ境界を比較している。

MSVC 19.44 / VS2022 / x64 Release / 静的CRT / assertions有効 / LTO無効。
Journey・周期バックエンド・OD assignmentを有効にし、CargoDist Asymmetric、
`recalc_interval=8`、`recalc_time=32`。保存・資産・設定は隔離したディレクトリを使用した。
入力セーブは変更していない。

比較用ソースは指定コミットの `git archive`。測定専用のfixtureコマンドに存在した
`ClientPoolID` と `ClientID` の比較のコンパイルエラーだけ、
`client->client_id != ClientID::Server` に修正した。通常の経路探索は同じベースである。
測定コピーのPE subsystemをconsoleへ変更し、stdin/stderrを収集した。

実セーブの既存NewGRF容量変更・Error pause条件を継続するため、双方の測定コピーは
`OPTION_PASSENGER_BENCHMARK=ON`。製品既定値はOFFで、製品回帰もOFFで行う。
大量の既存NewGRF警告は全文を作業ディレクトリに保持し、抜粋・行数・SHA-256を
測定成果へ保存する。実セーブ、NewGRF、実行ファイルはGitへ追加しない。

時間は `steady_clock` の経過時間で、OSのスケジューリングや同期待ちを含む。
ルーティングのWork admissionやタイブレークには使わない。
p95/p99は1msヒストグラムの上限値。プロセスメモリは100ms間隔の観測である。
既存の別ゲームプロセスが動作していたため、背景負荷を含む単回比較として扱う。
試作測定も区別して残し、最終結果には `baseline-long` と `schedule-final` を用いる。

## 最終結果

最終測定の数値は [集計JSON](measurements/passenger-journey-schedule-2026-10-13/summary.json)
と各console/processログに記録する。最終ソースの `schedule-final` を以下に示す。

| 指標 | ベース | Schedule-aware最終版 |
|---|---:|---:|
| 完了tick | 8,000 | 8,000 |
| 全体平均 ms/tick | 38.070 | 35.136 |
| 全体p95 / p99 ms（上限） | 55 / 65 | 66 / 89 |
| 全体最大 ms | 118.532 | 137.376 |
| Journey平均 ms/tick | 13.248 | 14.235 |
| Timed実乗車 | 11,607人 | 12,910人 |
| Timed実降車Transfer / 最終配送 | 0 / 7,497人 | 2,103 / 8,825人 |
| Schedule実乗車 | 0人 | 4,839人 |
| Schedule実降車Transfer | 0人 | 3,921人 |
| Scheduleから次車両への接続完了 | — | 2,882人 |
| Timedから次車両への接続完了 | — | 1,539人 |
| Schedule実利用後の最終OD配送 | — | 1,452人 |
| 最終車両がScheduleの配送 | 0人 | 0人 |
| Untimed実乗車 / 降車Transfer / 配送 | 562,737 / 288,508 / 225,076人 | 554,927 / 279,781 / 223,746人 |
| 最終配送合計（最後の車両で分類） | 232,573人 | 232,571人 |
| 末尾待機 | 272,312人 | 270,156人 |
| 末尾Schedule待機 / Timed待機 | — | 407 / 1,157人 |
| peak working set MiB | 1,944.37 | 1,965.38 |

Timed乗車は減っておらず、Scheduleの実乗車・接続完了と混在経路の最終OD到達を
確認した。総配送数はほぼ同じで、総配送量の増加や需要配分の改善を断定しない。
平均tick時間は約39msの範囲を大きく悪化させていないが、p95/p99と最大値は増えた。
平均値の差には背景負荷も含むため、統計的な高速化の証明には用いない。

Schedule ODキャッシュは72,813 hit / 45,383 miss（61.60%）、
検索経過時間45.974秒、論理Work 3,100,942,050。
採用9,915packet / 15,039人は全件 `timed_deferred=true` の暫定採用であり、
Timedが存在しないと判断した数値ではない。実車両・実指令の検証を通って積載した。
推定待機は8,323,238 person-ticks、実待機は244,589 person-ticks（4,839人の乗車観測）。
保守的な未知位相モデルが、到着中・近接便への即時積載では待機を大きく過大評価した。
この差を確定発車時刻や架空の予約へ変換していない。

ServiceGraph構築・物理Trip予測は双方0回。
DB公開・OD検索・Journey tick負荷はconsoleログの専用行で確認できる。
途中標本では1,526tickで接続完了26人・Schedule-assisted配送0人、
3,746tickで780人・103人、6,040tickで1,935人・804人と進んだ。
短期の0配送を成功として報告せず、8,000tickの実配送まで継続した。

`transferred` は実際に車両から駅へ移った人数、`Completed interchanges` は
その後に次車両へ実積載した人数。予約・経路割当・stagingだけでは増えない。

最終車両の方式による Timed / Schedule / Untimed の配送計数と、
途中に実際のSchedule区間を含んだ `Schedule-assisted final OD` を区別した。
後者は前者と重複するため、配送合計へ加算しない。
短期ではSchedule最終車両の配送が0でも、乗換時にTimedへ再評価する混在経路がある。
履歴は予約時には立てず、実積載したcargoの到着・staging時に記録し、
分割・結合・保存・ロードで維持する。長期測定で最終OD配送まで追跡する。

## 必須ケースの検証対応

| 要求 | 回帰 |
|---|---|
| 1 完全ダイヤ・位相不明直通 | duration-only直通、4実車種の予約・配送 |
| 2 複数系統乗換 | 4実車種を経由する3回の実乗換と配送 |
| 3 普通・特急比較 | riding / dwell / wait / capacityコストの比較 |
| 4 低頻度特急・高頻度普通 | 低頻度・低容量特急が負けるケース、観測位相の最大間隔 |
| 5 Timed優先 | 高速な未知位相便があっても有効なTimed ODを採用 |
| 6 Timedなし・Scheduleあり | 位相不明のDBからEstimated Intentを採用 |
| 7 Scheduleなし・Untimedあり | 不完全ダイヤから実LinkGraph ODで積載 |
| 8 既知・未知混在 | Schedule→Timedの実乗換・最低時間・最終配送・履歴保存 |
| 9 別実車両 | 共有別車両への予約・部分積載・個別Binding |
| 10 実乗換・最低時間 | 早すぎる積載拒否、到着系統除外、Fallback後の再乗換 |
| 11 Forced Unload / Transfer | 実強制降車と次Intentの生成、through検証の拒否 |
| 12 NoLoad / NoUnload | コンパイルmaskと実乗車prefix検証 |
| 13 空席・部分積載・予約 | 既存座席割当、各車種の100人予約と20人ずつ積載 |
| 14 削除・編成・指令変更 | reverse index、容量失効、現行指令prefix、車両・OrderList ID再利用 |
| 15 generation変更 | 古いIntentの乗車拒否と再探索、実車の安全な継続 |
| 16 分割・結合・保存 | 同一Intentの結合、異なるgenerationの結合拒否、旧wire・現行cargo保存 |
| 17 決定性 | 冷/暖/破棄/保存DBの結果とWork一致、RNG非変更、loopback参加 |
| 18 大規模 | 8,000駅・4,000車両・4,096 OD再利用、既存の大規模混合車種回帰 |

実際の暗黙停車からの継続はlive validationだけで許可する。
予測APIの既定動作は引き続きその未宣言の現在Occurrenceを拒否し、
DBには将来の暗黙停車を追加しない。

## 制限と残る観測範囲

部分ダイヤ、unsupported controlの区間分割、需要に応じた均衡配分、混雑の将来予測は未実装。
最低容量リスク・未知頻度のペナルティは決定的な選好モデルで、運行間隔や座席の保証ではない。
固定Work・label・memory上限は全ネットワークの最適性を保証しない。
expiry後に同じ有効な最良系統を再選択することもある。

全世代更新時のODキャッシュ破棄は残る。古い候補の無条件再利用による
冷/暖・保存/ロード間のWork不一致を導入していない。
製品構成 `OPTION_PASSENGER_BENCHMARK=OFF` で **CTest 495件すべて成功**。
`[passenger]` は **282ケース / 970,671 assertions成功**。
ログは測定ディレクトリへ保存した。旧/currentセーブ、GUI・診断のsave purity、
無効時動作、RNGとWork一致の既存回帰も含む。

私設loopback `127.0.0.1:17493` にNewGRFなしの実道路・実バス・50人の既知ODを用意し、
Dispatch有効・slot未割当として、完全ダイヤの位相を未認証のまま運行した。
DB公開前に初期参加、その後に途中参加し、2クライアントのCPU affinityを実際にmask 1へ制限。
ホストは制限せず、`network.sync_freq=1`。
`13639680→13642032` の2,352tickを進め、終了時点でも両クライアントの接続を確認し、
同期不一致を検出しなかった。公開1系統・known phase 0・実Schedule乗車31人、駅残留19人。
この区間の配送は0人であり、配送成功とは報告しない。
[同期結果](measurements/passenger-journey-schedule-2026-10-13/mp-result.json)とログを保存した。

この同期試験はNewGRFなしの制御ケースである。
既存の大型NewGRF実セーブには、ベースでも過去に途中参加1tick後のSTATION乱数不一致が
記録されている（[前回検証](passenger-journey-periodic-2026-10-12.md)）。
今回その大型セーブの途中参加が成功することは検証しておらず、
WAN・全車種の長期同期まで保証する結果ではない。

再実行には [測定スクリプト](measurements/passenger-journey-schedule-2026-10-13/Measure-Schedule.ps1)、
[集計スクリプト](../utils/passenger_schedule_benchmark.py)、
[loopbackハーネス](measurements/passenger-journey-schedule-2026-10-13/Exercise-Schedule-Multiplayer.py)
を用いる。loopback fixtureの道路・車両・旅客準備は、接続前の隔離マップだけを変更する。
