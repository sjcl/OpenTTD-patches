# Work Budget 最終修正・検証

この記録の統合保留判断は2026-10-14時点のもの。以後、ユーザー指定により全cache無効時のtimeoutを独立課題へ分離し、最新 `jgrpp-ya` に追従した。[2026-10-15の統合検証](passenger-query-work-admission-integration-2026-10-15.md)を現在の判断に用いる。以下の測定値・失敗原本を、新ベースの結果へ読み替えない。

対象は `feat/passenger-query-work-admission`、依頼ベース `a77dca75a2`、時刻修正の実装SHAは `c16da03990eff522af2abf0ca9379ce5d24e5aac`。Work予約・実使用精算、16 OD/tick、Timed 49,152 / Schedule保護16,384、1 OD 65,536、Global 1,048,576 Work/tickは変更していない。Roundsは導入していない。

**統合判断は保留。** 修正・性能再測定・実v4互換・製品構成の全回帰は成功した。一方、実マップで両クライアントの派生キャッシュとScratchを無効化した長区間loopbackでは既存の受信タイムアウトが発動し、接続維持条件を満たせなかった。短区間の完全Cargo一致とofflineのSave一致を、長区間の途中参加成功へ読み替えない。

追加の[切断原因分離](passenger-query-work-admission-network-cause-2026-10-14.md)では、サーバー送信継続とclientの受信待機データを確認した。無競合の2再現でclientの一括catch-upが23〜24秒を占有し、その96%以上がゲームthreadのCPU時間だった。本切断は受信処理へ戻れない機構によるもので、desync検出による切断とは区別した。タイムアウトは延長していない。

## 時刻と診断

`GetPassengerScheduleIntentAge()` はEstimatedServiceIntent / EstimatedBoundServiceだけを受け付け、`INVALID_STATE_TICKS = INT64_MIN` と未来の計画時刻を除外する。順序確認後、両オペランドをuint64_tへ変換して減算する。変換と減算は2^64を法とするため、数学的な非負差分がINT64_MAXを超えても正確に表せる。無効センチネルを除いた最大差 `INT64_MAX - (INT64_MIN + 1) = UINT64_MAX - 1` も判定できる。閾値が負ならfalse、計画時刻0は有効のまま。

同じ符号付き減算が実乗車時のSchedule待機集計にもあったため、同じ年齢計算へ統一した。無効観測をsampleに入れず、極端な人数加重値とtick間の合計はUINT64_MAXで飽和する。診断以外の探索・期限判定は変更していない。

MSVC境界回帰327アサーションと、実製品ヘッダーを使うGCC 16.1 UBSan trapモード95ケースが成功した。通常のlibubsanは未搭載、WSL仮想化も無効だったため、ランタイム不要の `-fsanitize=undefined -fsanitize-undefined-trap-on-error` を使用した。旧形式の危険な減算はillegal-instructionで停止する負の校正も確認済み。全種別・0・4095/4096/4097・符号境界・負同士・負から正・閾値0/負・無効センチネルと保存状態不変性を検証した。

## 修正後の実マップ性能

[6ランの原本・集計・実セーブ再hash検証](measurements/passenger-query-work-admission-2026-10-14/final-review/summary.json)。入力SHAは `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`、16,384×4,096、StateTicks `3176545487 → 3176553679`、各8,192実tick。同じNewGRF・baseset・config、MSVC 19.44 x64 Release `/O2 /Ob2 /MT`、assert ON、LTO OFF。共有ロック、各ラン前90秒の無競合、実行中5秒ごとの競合監視を使用。順序はA/B/C/C/B/Aで、各2ラン、競合による除外0。

Aは `8a96044d40826dbe01daa8343d99089353fe3568` のActiveContextを共通の読取専用計測ビルドへ差し替えた旧一括課金。plannerとPacketの製品実装はそのベースと一致する。共通の診断表示・時刻ヘルパー・v4読込許容は現行を使用し、旧版の別ビルド全体との厳密なバイナリ比較とは区別する。Bは修正版warm、Cは同一Bバイナリでpolicy・Departure Index・resident fallback cacheを無効化。イベント共有とScratch再利用はCでも維持し、Scratch無効は途中参加で追加検証する。

| 条件 / 実行順 | Simulation平均 ms | p50 / p95 / p99上端 ms | Simulation最大 ms | Journey平均 ms | Journey p95 / p99上端 ms | Journey最大 ms |
|---|---:|---:|---:|---:|---:|---:|
| A / 1 | 32.197 | 31 / 49 / 63 | 102.605 | 13.621 | 30 / 44 | 84.128 |
| B / 2 | 32.460 | 31 / 49 / 60 | 74.522 | 13.774 | 30 / 40 | 56.967 |
| C / 3 | 47.393 | 46 / 70 / 81 | 105.637 | 28.865 | 51 / 62 | 82.991 |
| C / 4 | 47.535 | 46 / 71 / 82 | 112.205 | 29.053 | 51 / 63 | 89.917 |
| B / 5 | 31.611 | 31 / 47 / 58 | 86.136 | 13.334 | 28 / 38 | 65.681 |
| A / 6 | 32.204 | 31 / 50 / 63 | 97.002 | 13.614 | 30 / 44 | 76.197 |

2ラン平均はA 32.2005 / B 32.0357 / C 47.4637 ms/tick。B/Aは−0.512%だが、Bのラン間幅は平均比2.65%なので速度向上とは断定しない。2反復だけから一般的な信頼区間を作らない。quantileはnearest-rank、1ms bucket上端であり、exact値ではない。各ランのSimulation/Journey最大値、全histogram、CPUとメモリは原本に保存した。

Timed CPUはGetThreadTimesによるsimulation threadのuser+kernelを各公開Plan呼出しの前後で観測した。A 54.359 / 53.594秒、B 61.859 / 60.422秒、C 111.141 / 111.563秒。これは粗いOS会計の短い呼出しへの帰属で、elapsed planner時間と混同しない。QueryThreadCycleTimeも保存し、A 327.912 / 326.772 billion cycles、B 287.442 / 284.982、C 475.582 / 476.120。CPU秒・cyclesの局所指標が逆方向であるため、cyclesを一定周波数で秒へ換算したり、Timed単独の高速化を主張しない。統合判断には全simulationと全プロセスCPUを重視する。

全プロセスCPUはA 362.703 / 362.781秒、B 365.016 / 360.156秒、C 486.750 / 488.813秒。ロード・保存・workerを含む。A/B平均差は約−0.04%。peak Working SetはA 2,091,683,840 / 2,087,518,208 bytes、B 2,091,397,120 / 2,085,220,352、C 2,061,717,504 / 2,063,310,848。sampled peak privateはA 2,089,132,032 / 2,085,675,008、B 2,089,172,992 / 2,083,229,696、C 2,059,272,192 / 2,060,861,440 bytes。100ms samplingのためprivateの真の最大とは区別する。

## Work・実輸送・Packet

以下は各条件の2ランで同じ値だった。A/Bはfair shareと結果の採用によってOD列そのものが変わるので、同一問い合わせ列への検索品質の比較ではない。

| 指標 | A 旧一括課金 | B 修正版warm / C cold |
|---|---:|---:|
| 受付OD | 131,072 | 130,722 |
| 受付拒否 | 8,119,999 | 3,579,533 |
| 予約Work | 8,589,934,592 | 8,566,996,992 |
| 課金Work | 8,589,934,592（一括） | 3,333,525,716（実使用） |
| 実論理探索Work | 4,546,682,879 | 3,333,525,716 |
| 未使用返却Work | 0 | 5,233,471,276 |
| 最大受付/tick | 16 | 16 |
| 最大課金/tick | 1,048,576（一括） | 1,040,512（実使用） |
| Timed Certified / Provisional | 53,511 / 4,854 | 64,129 / 5,750 |
| Timed Work終了・完全経路なし | 38,817 | 26,860 |
| Work後のSchedule救済 | 0 | 10 |
| 両経路なし / 受付 | 38,817 / 131,072 (29.615%) | 26,850 / 130,722 (20.540%) |
| Timed 乗車 / 乗換 / 配送 | 56,412 / 26,434 / 17,876 | 101,158 / 41,371 / 37,304 |
| Schedule 乗車 / 乗換 / 配送 | 88 / 26 / 36 | 294 / 142 / 131 |
| Untimed 乗車 / 乗換 / 配送 | 543,419 / 267,082 / 223,984 | 502,638 / 251,169 / 204,285 |
| 全方式の最終配送 | 241,896 | 241,720 (−0.073%) |
| Packet Pool終点 / 実最大 | 194,857 / 194,887 | 194,422 / 194,464 |
| Save bytes | 29,537,908 | 29,798,408 (+0.882%) |
| 終点の既知OD待機人数 | 190,159 | 184,142 |
| Schedule intent age ≥4,096 | 0 | 0 |

Aの拒否理由はQueryWork 8,119,999 / QueryLimit 0で、旧制御のGlobal/Event/Fairの細分化は行わない。B/CはTickQueries 1,321,679、FairWork 82,065、FairQueries 2,175,789、EventQueries/EventWork/GlobalWorkはいずれも0で、合計が拒否総数と一致する。Timed配分6,425,247,744・使用2,285,827,472、Schedule配分1,493,499,270・使用1,047,698,244 Work。Schedule配分は実行時に未使用Timed枠を貸し出した値で、独立した追加予約ではない。

AのWork後Schedule救済0は、Scheduleのtimed_deferred packet数が0である観測から導出できる。そのためこの区間ではTimed Work・完全経路なしの数が最終的な両経路なしと一致する。Work終了率24.933%と両経路なし率20.540%は別の指標。

Packet数は増えず、最終配送はほぼ維持した。Saveサイズの+0.882%は隠さず評価する。Work経験旅客の長期追跡は廃止済みで、この人数を0として報告しない。旧 `intent_wait_ge_4096` と旧Work-cohort transportは一切使わない。Schedule専用のageが0であることを、Timedも含めた長時間待機人数0とは解釈しない。

## キャッシュ決定性

4つのB/Cランで受付順序・検索入力順序・経路結果の明示的フィールドFNV-1a digest、論理Work、拒否理由、実輸送、Packet終点/最大数が一致した。実ファイルを再hashし、全Save SHA-256も `5288d93491a47f9c60067ed0aac4d952307b1a0e31eab859901caa0463fa689d` で一致した。digestだけを暗号学的な証明とせず、全Saveと輸送・会計の一致を併用した。タイマーとdigestはコピーした計測TUのみで、Workや停止条件へ入力していない。

## 実v4セーブ互換

[実ファイルの照合結果](measurements/passenger-query-work-admission-2026-10-14/final-save-review/result.json)。旧実行ファイルはf519df10abからの固定a1492347...で、製品ソースは11730ea682と一致する。既存の実v4 Save SHA `2fa1b07407db3ca3077664fb9aab21a13378d2df6d478c80d4cf81302495f1c2` を再利用した。

旧タグtrue 11,703 / false 182,080 Packet、計193,783 Packet・855,403 cargo単位。読込後も同じ数で、既知OD 299,288単位・Journey 19,802 Packetを保持した。支払情報の検証も成功し、Packetの全保存対象フィールド・OD・Intent・支払のfingerprintが、値を一時観測する版、製品と同じ未知フィールドskip版、v3再読込で一致した。前2版のv3 Save全体SHAも一致した。履歴が異なっていた225組は通常のStation/Vehicle両方のmerge条件を満たす。新しくPacketフラグや保存フィールドを再追加していない。

実ファイルのSLXIを独立に読み、旧入力v4、新出力v3を確認した。再保存v3からStateTicks `3176553679 → 3176553935` の256実tickを継続し、支払検証成功・Error pause解除0・assertなし。

初期の検証起動へ誤って `-Q`（NewGRFスキャン省略）を含めた試行では、旧/新両方で同じcargo正規化とroad speed cache assertが起きた。通常スキャンへ戻すと、人数・Packet数の保全と継続が成功した。その試行は正式結果から除外し、理由とcrash原本を保存した。assert抑制・車両設定変更・経路変更で通した試験ではない。

## 途中参加・製品構成・統合判断

[ネットワーク検証原本](measurements/passenger-query-work-admission-2026-10-14/final-network-review/)には、成功した短区間と失敗した長区間を別々に保存した。完全stderrはgzip、読めるnetwork-eventsとconsole・Cargo snapshotも併存する。NewGRFの既存capacity警告を除外して成功扱いにはせず、原本をすべて保持した。

共通条件は実入力マップ、loopbackサーバー1台・初期クライアント1台・途中クライアント1台、sync_frequency=1、通常のCPU affinity。両クライアントのpolicy・Departure Index・resident fallback・Scratchを無効化し、scratch retained bytes=0を短区間で確認した。`max_lag_time` / `max_join_time` は既存ハーネスと同じ32,000で、クライアントの20秒受信監視は変更していない。

| 試験 | 結果 |
|---|---|
| 途中参加後25秒、全体1,135実tick | 初期・途中参加成功、2台が接続維持。サーバー `3176545487 → 3176546622`、途中参加後714実tick進行。3台の同tickで115,244 Packet・842,760 cargo単位・既知OD154,843単位・Journey7,201 Packet、全保存対象フィールドdigestと支払digestが一致、支払検証成功、assert/desyncなし。 |
| 連続120秒、全体3,963実tick | 両参加成功後に両クライアント切断。server終点 `3176549450`。初期clientはframe 1,768、途中clientはframe 2,228で`ClientQuit`。継続成功として扱わない。 |
| 初期の120秒試験2回 | 同様に参加後切断。進行待ちtimeoutとなった失敗を保持。 |
| 受信監視の追加観測 | 初期clientがframe 2,039で、last_packetから24,697ms経過。製品と同じ `CheckConnection()` の20秒分岐が発動し、`ClientQuit`で閉じた。切断を検出して試験を失敗終了した。 |

長区間でWork・経路由来のsync error、assert、異常終了のログは観測しなかったが、切断は実際に発生した。[追加の原因分離](passenger-query-work-admission-network-cause-2026-10-14.md)で、サーバーが送信を継続する間の23〜24秒の一括catch-upと、client socketの到着済みデータを確認した。旧Admissionの同じネットワーク条件での対照試験はしていないため、今回のWork変更が発生頻度へ無関係とは確定しない。タイムアウトの延長・sync頻度の低下・人工的なpauseで失敗を隠していない。ネットワーク全体の修正はこの診断修正へ混ぜていない。

[製品構成・回帰ログ](measurements/passenger-query-work-admission-2026-10-14/final-checks/result.json)。MSVC x64 Release `/O2 /Ob2 /MT`、assert ON、LTO OFF、`OPTION_PASSENGER_BENCHMARK=OFF`で製品・テスト・regression_filesをビルドした。プロジェクトにコピーした計測TUや `WITH_PASSENGER_BENCHMARK` が残っていないことを検査した。全CTest 590/590成功、旅客回帰352ケース・2,670,223アサーション成功、会計証拠のPython回帰8/8成功。Work早期return/例外返却・部分予約・65,536/1,048,576/16件上限・Fair Share・保護枠・キャッシュ・split/merge・車庫/運転停車/強制降車・列車/バス/船/航空機・保存/同期の既存回帰を含む。大きな診断追跡をPacketへ戻しておらず、CargoPacket製品ソースは比較ベースと一致する。

CPU・配送・Packet・保存互換の条件はこの区間で満たしたが、マルチプレイ接続維持条件が未達のため `jgrpp-ya` への統合を承認しない。次の確認は、同条件の旧Admission対照と、catch-upから定期的に受信へ戻る別修正の長区間loopback。cache有効の120秒対照が成功しても、要求されたcache無効試験の代わりとはしない。`jgrpp-ya` への自動マージは行っていない。
