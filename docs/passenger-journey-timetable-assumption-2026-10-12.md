# JourneyPlanner のダイヤ遵守仮定（2026-10-12）

ベースは `jgrpp-ya` の `1556b330cd9360d1e2f6b0ad50e45b70b86c6341`。
作業ブランチは `feat/passenger-journey-timetable-assumption`。

## 予測ルールと実行

`PassengerTimingPolicy::{Conservative, AssumeTimetable}` を共通予測器に追加した。
既定値は Conservative とし、従来の PassengerRoutingContext と Journey 無効時の
CargoDist を維持する。Active の scoped ServiceGraph、名目 Service パターン、
物理 Trip、乗車・継続検証、Shadow の Planner 評価は AssumeTimetable を明示する。
同じ停止イベントのパターンキャッシュにも policy を含め、両方針の混用を防ぐ。

AssumeTimetable の駅停車では、4種類すべての OrderLeaveType、Full Load/Any、
貨物別指定、他の貨物の満載待ちを理由に時刻を Unknown にしない。
設定済みの Wait Time を使い、通常の lateness_counter で短縮・延長しない。
将来の Travel Time も設定値を使う。未設定フラグは、非ゼロの実測値が残っていても
未設定として扱う。明示的に設定した 0 ticks は既知であり、未設定の 0 と区別する。

現在の走行・積載指令だけは経過時間を差し引き、残り時間を0未満にしない。
現在走行中で設定時間を超過した場合は、未到着の駅への即時到着を生成しない。
scoped 予測はその未知区間で停止し、名目走査では後続区間の設定所要時間を保持しても
絶対時刻を確定しない。現在停車中の到着時刻は元から生成しない。

Scheduled Dispatch の現在割当済みスロットは lateness に符号化されたオフセットを
引き続き使用する。将来のスロット選択と共有車両の競合も従来どおり検証する。
Conditional、Depot、Slot/Counter、Refit、施設・指令進行の問題を一律に無視しない。
OT_IMPLICIT のスキップ、未知の臨時停車を予測しない方針も維持した。

乗車検証の独立した128 Visit上限を廃し、Active Graph と同じ4,096 Visits/stepsを使用する。
容量・施設・指令状態・署名計算と予測には2,097,152 workの共通した上限を使う。
VehicleID/OrderListID、指令順、停車設備、署名、正しい発着 occurrence、NoLoad/NoUnload、
途中での強制乗換はそのまま検証する。発着時刻の無条件な補正や許容幅は追加しない。
通常の lateness 変化だけでは拒否せず、実際の早発・遅延で occurrence が変われば
ライブ状態から再計画し、失敗・予算不足時は既存の保留/Fallbackへ移行する。

車両の運行制御、実容量、空席、予約、部分積載、Gradual Loading は変更していない。
受入可能な最終ODでは Transfer-only でも配送できる現行仕様を維持する。
新しい save field、CargoPacket field、乱数呼出しは追加していない。

## 検証

MSVC 19.44 x64 Release、assertions ON、LTO OFF。
`openttd_test [passenger]` は **209 cases / 937,813 assertions** が通過した。
全 CTest は **424/424** 通過。最初の本番セーブ読込3件は実行ファイル横の
ベースセット/言語不足で失敗し、同じアセットを配置後、3/3通過した。
測定専用処理を除いた製品の本番セーブ・ロード3件も再確認した。

* 早発4種と Full Load/Any/別貨物 Full Load の全組合せが通常と同じ発着予測になる。
* 未設定/設定済み0、現在経過、早着/遅延、超過走行と後続区間、次周回の予測。
* 名目パターン/物理 Trip/scoped capture と policy 別キャッシュの整合性。
* Conditional/Depot と Scheduled Dispatch の将来/割当済み/競合を両policyで検証。
* 141番目の指令で降車する長いJourneyの実積載。128 Visitで拒否されない。
* ダイヤ仮定で作った便の予約・部分/段階積載、実乗換、Transfer-only最終ODでの実配送。
* 通常のlateness変化での既存Journey継続、実際の早発/遅延でのライブ再計画。
* 既存の普通/特急比較、四交通モード、指令・暗黙停車変更、NoUnload/強制乗換、
  満員/予約失効、save/load、RNG、冷/温cache・固定予算の決定性、無効時の同一性。

生出力は [旅客テスト](measurements/passenger-journey-timetable-assumption-2026-10-12/passenger-tests.log)、
[全CTest](measurements/passenger-journey-timetable-assumption-2026-10-12/ctest.log)、
[アセット配置後の再実行](measurements/passenger-journey-timetable-assumption-2026-10-12/ctest-retry.log)、
[測定後の製品検証](measurements/passenger-journey-timetable-assumption-2026-10-12/restored-product-tests.log)。

## 実マップ測定

既存調査と同じ入力 SHA-256
`14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`。
隔離コピーを使い、元入力のhashは不変。両側とも新規プロセスで直列に測定した。
null:until_exit/null/null、PAX distribution 2、OD/Journey ON、recalc 8/32。
State ticks **3176545487 → 3176547014** の1,527 ticksで、最後の部分tickを除いた
時間サンプルは両側 **1,526**。ロード/保存を含むプロセス時間とsimulation時間を区別する。

既存 NewGRF の容量変更等の警告は両側に残る。console unpause時に Error pauseを
解除する測定専用処理だけを両バイナリへ加え、元セーブを変更しない。
製品ソースの自動復元・hash一致・再生成を確認した。
基準の適用22,414人、乗車7,673人、配送1,731人は前回のpragmatic測定と一致した。
基準commitには共有capture/index改善も統合済みなので、以前の報告の時間を今回の基準に
流用しない。以下は今回の同条件の単一ペアであり、実時間は環境変動も含む。

| 指標 | 基準commit | ダイヤ遵守仮定 |
|---|---:|---:|
| JourneyPlanner検索数 | 22,809 | 22,783 |
| 時刻付きJourney適用人数（再計画含む） | 22,414 | 30,781 |
| 時刻付き適用packets | 9,461 | 12,423 |
| Untimed Fallback採用（停止イベントの判断） | 23,892 | 23,593 |
| 時刻付き実乗車人数 | 7,673 | 11,226 |
| 時刻付き実乗換人数 | 1 | 21 |
| 時刻付き最終配送人数 | 1,731 | 2,371 |
| 時刻なし実乗車人数 | 81,242 | 78,059 |
| 時刻なし実乗換人数 | 39,405 | 39,653 |
| 時刻なし最終配送人数 | 31,078 | 29,981 |
| Timingによる予測停止（累計Trip観測） | 18,873,731 | 7,152,668 |
| ServiceGraph構築回数 | 23,464 | 23,409 |
| ServiceGraph論理Work | 13,413,611,618 | 13,463,777,302 |
| ServiceGraph構築時間 | 229.057 s | 224.693 s |
| Query入場/署名Work | 1,524,315,024 | 1,529,386,216 |
| Query検索時間 | 10.037 s | 19.341 s |
| 平均simulation tick | 235.317 ms | 234.132 ms |
| 最大simulation tick | 420.839 ms | 448.633 ms |
| 全simulation tick時間 | 359.094 s | 357.286 s |
| 最終総待機人数 | 211,069 | 211,083 |
| 最終時刻付き待機人数 | 2,606 | 3,688 |
| Graph/index論理メモリ最大 | 36.5 MiB | 40.4 MiB |
| Query scratch最大 | 1.1 MiB | 1.2 MiB |
| プロセスpeak working set | 1,910.0 MiB | 1,920.5 MiB |
| サンプリングpeak private bytes | 1,911.0 MiB | 1,921.4 MiB |
| プロセスCPU時間 | 491.000 s | 465.750 s |
| プロセスwall時間 | 375.507 s | 372.428 s |

時刻付き適用は **37.3%増**、実乗車は **46.3%増**、時刻付き配送は **37.0%増**。
実乗換も1→21人となった。Timing停止は **62.1%減**、Untimed採用は299判断減った。
延長した予測が後続の制御等へ到達するため、path不確実性の累計は
2,976,081→3,853,615に増えている。未知制御を既知に置き換えた結果ではない。

ServiceGraph Workは **0.4%増**、Query検索時間は **92.7%増**（10.04→19.34秒）。
Graph構築時間は229.06→224.69秒。平均tickは235.317→234.132msで **0.5%減**に
とどまり、環境変動の範囲を超える速度改善とは主張しない。
最大tickは420.839→448.633msで **6.6%増**、Graph/indexの論理メモリ最大も
36.5→40.4MiBに増えた。プロセスpeak working setは **0.6%増**。
プロセスCPU時間は5.1%減だが、平均tick同様、単一ペアの観測である。

時刻付き/なしを合計した最終配送は **32,809→32,352人**で、457人（1.4%）減った。
総待機は211,069→211,083人とほぼ変わらず、全輸送・待機の改善は確認できない。
時刻付き配送の増加を全旅客の改善と読み替えない。
PredictionIncomplete Fallbackは20,312→15,317 packetsに減る一方、
QueryWork Fallbackは2,332→3,871 packets、QueryWork deferredは440,416→444,420に増えた。
時刻付き候補が増え、検索・再計画・予算下の処理配分が変わる負荷と制約が残る。

適用は再適用・再計画を含む。乗車・乗換・配送は実CargoLoad/Transfer/Deliveryの人数で、
旅客の一意件数ではない。Untimed採用も人数・予約成功数ではない。
Timing停止はGraph構築ごとの累計観測で、実車両数ではない。
Query Workは既存の固定入場残高と署名計算のdebitで、実走査Workの総計とは異なる。

### Service 1 / Vehicle 3355

現在のtile/指令7/経過276/lateness -7、全13駅指令の設定済みフラグを同じ入力で確認した。
Conservativeは Order 9 / Station 37 の `OLT_LEAVE_EARLY` で Timing停止し、
scoped接頭部は3 Visits、確定境界 `3176547408`。
AssumeTimetableはこの駅の設定Wait Time 77 ticksを使い、4,096 ticksの期間を
**Complete / 4 Visits / 確定境界3176549583** とした。次指令11の到着は期間外。
旧診断と、新バイナリでのConservative結果も一致し、policyの選択による変化を確認できる。

[修正前console](measurements/passenger-journey-timetable-assumption-2026-10-12/before-console.txt)、
[修正後console](measurements/passenger-journey-timetable-assumption-2026-10-12/after-console.txt)、
[条件・hash・全数値](measurements/passenger-journey-timetable-assumption-2026-10-12/metadata.json)、
[測定用生成処理](measurements/passenger-journey-timetable-assumption-2026-10-12/Build-Measurement.ps1)
を保存する。大きなセーブ、アセット、バイナリは `build-timetable/run/` に置きGitには追加しない。

## 残る制約

設定済みダイヤは予測上の仮定であり、実際の接続・発車・空席を保証しない。
超過した現在走行区間、未設定時刻表、未対応制御、競合するDispatchはUnknownのまま。
実際の発着ずれや余分な停車で保存occurrenceが合わない場合、再計画できなければ
Fallback/保留になる。固定Work、memory、Visit/step、query/labelの上限も維持している。
実際のマルチプレイ連続運転、GUI描画の目視、旅客単位の旅行時間・一意人数は未測定。
決定性はRNG、save/load、固定予算、冷/温cache一致の自動検証の範囲で確認した。

## 変更ファイル

* `src/passenger_routing.cpp/.h`: policyと共通予測、旗の確認、遅延/早発/満載待ちの扱い。
* `src/passenger_service_graph.cpp/.h`: 名目/物理予測へのpolicy伝播、パターンcacheの分離。
* `src/passenger_journey_active.cpp`: scoped計画と乗車/継続検証の方針・上限を統一。
* `src/passenger_journey_shadow.cpp`: ShadowのPlanner側をダイヤ仮定へ統一。
* `src/console_cmds.cpp`: 両policyの比較診断。製品に警告解除処理は残さない。
* `src/tests/passenger_routing.cpp`, `passenger_service_graph.cpp`, `passenger_journey_active.cpp`: 回帰検証。
* `docs/passenger-journey-active.md`, `passenger-journey-planner.md`, `passenger-journey-shadow.md`,
  `passenger-service-graph-scaling.md`: 現行仕様の説明。
* 本レポートと測定ディレクトリ: 条件、hash、生ログ、検証・再現用スクリプト。
