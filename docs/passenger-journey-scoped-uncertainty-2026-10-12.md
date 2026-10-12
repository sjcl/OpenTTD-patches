# JourneyPlanner: OD単位の不確実性と実マップ（2026-10-12）

ベースは `fix/passenger-journey-real-map-scaling` の
`06350758d2b91cd2e74ee2da23a8dd08f4fb98b0`、実装ブランチは
`fix/passenger-journey-scoped-uncertainty`。
前回の[実マップ](passenger-journey-real-map-2026-10-12.md)と
[スケーリング測定](passenger-journey-real-map-scaling-2026-10-12.md)を踏まえた変更。
測定原文・ハッシュ・設定は
[測定metadata](measurements/passenger-journey-scoped-uncertainty-2026-10-12/metadata.json)に保存する。

## 運行モデルとOpaqueの原因

今回の依頼で指定された運行モデルを統一した。明示指令に従って運行すると
仮定し、未知の将来の臨時停車も、履歴として既に存在する `OT_IMPLICIT` も
予測上はスキップする。予測器、ServiceGraphの停車駅、構造インデックス、
OD-aware Fallbackの指令走査は同じ規則を使う。道路が実際に接続しているかは
今回の予測可否判定に追加しない。互換施設・乗降制約の既存チェックは維持する。
Completeはこのモデルと予測期間・評価条件における認証であり、実運行で余分な
停車や遅延が生じないという保証ではない。

読み取り専用の `passenger_service_inspect <vehicle>` で、前回と同じ入力コピーを
state tick `3176545487` において確認した。
[指令・予測の原文](measurements/passenger-journey-scoped-uncertainty-2026-10-12/inspect-console.txt)。

* **Service 714 / Vehicle 16563** は道路車両で、Order 0が `OT_IMPLICIT` /
  Station 4323、Order 1が `OT_IMPLICIT` / Station 4320。明示指令はゼロ。
  tileは41025910、current orderは `OT_NOTHING`、real indexは0、implicit indexは1、
  timetable indexは65535、elapsedは1397523164、last stationは4323。
  `GetPassengerRouteState` が `OrderProgress` で失敗するため、予測器が実際に
  処理して停止したOrderIDは存在しない。旧実装の `implicit && !manual_stops`
  による `unbounded` 分類は、このサービスをOD接続にかかわらず強制収録し、
  全体をOpaqueとして中断した。今回の明示指令モデルでは停車駅集合が空で、
  対象ODへの接続はない。除外の根拠は道路の非接続や時刻表の推測ではない。
* **Service 1 / Vehicle 3355** は列車で、13個の明示駅指令がある。
  駅列は `13,20,35,37,39,42,43,42,39,37,35,20,13`。全指令のtravel/waitは
  timetabled。tileは22301316、current/real/implicit/timetable indexは7、
  Station 42へ向かう途中でelapsed 276、lateness -7、last station 43。
  最初の時刻不確実性は **Order 9 / Station 37の `OLT_LEAVE_EARLY`** による
  早発可能な待ち時間。暗黙停車でも未設定時刻表でもない。
  従来の通常予測は名目一周を続け、Order 6でTiming停止、13 visits、
  cursor `3176558424`、certified_until `3176547408`。
  新しいscoped予測はOrder 9で直ちに止まり、3 visitsと同じ確定境界を保持する。
  前回の最初の測定は1 tick後のため、境界が `3176547409` だった。

`fleet.implicit` / `fleet.unbounded` によるOpaque拒否は除去した。
一方、時刻不明はTiming、条件・Depot・Slot/Counter・Refit・指令進行などの
未対応制御は別の不確実性として残る。指令/位置/最初の未知Orderと理由の
診断値は、分岐・予算・RNG・保存データに使わない。

## 完全性と最適性の認証

`GetQueryStatus()` のCompleteは対象ODに必要な**候補の収録**が完了した意味に
限定した。未知の候補も収録する。`GetCoverage()` は時刻の不確実性があれば
Incompleteのまま。Tripの完全性、選択した物理乗車区間の完全性、ODの最適性は
別に扱う。

scoped予測は最初の未知時刻・制御で止まり、それ以前の確定発着時刻を保持する。
Plannerはまず確定した時刻・乗降条件の候補を検索する。Tripの後半がUnknownでも、
確定区間のlegは利用できる。候補が見つかっただけでは結果をCompleteにしない。

全未知候補を含む二状態の時刻探索を追加した。状態は「確定区間のみを使用」と
「未知区間を一度以上使用」。未知サービスは、全明示停車駅の任意の組合せを、
`max(その駅へ到達可能な時刻, 全車両中で最も早いcomplete_until)` 以降に
所要時間0で接続する。未知区間の方向・NoLoad・強制降車・容量・乗換待ち・
非負ペナルティを緩和し、既知の接続も容量・乗換費用を楽観的に扱う。
確定境界より前に乗り、未知区間まで乗り続ける可能性も含む。
`stopped_at` や名目所要時間は保証された最小時間に使わない。

この緩和で得た未知経路の最早到着は、実候補より良いか同等の安全な下界となる。
以下の場合だけ不確実性を解消する。

1. 緩和しても未知区間を使って目的地へ到達できない。
2. 要求されたK候補が全て見つかり、未知到着下界の経過ticksが、返す候補の
   最も悪い**一般化費用**より厳密に大きい。

全ペナルティが非負なので、2なら未知候補の最良スコアでも選択を変えられない。
等号はUnknownとする。到着時刻、乗換数、容量確度、VehicleID、occurrenceの
既存タイブレークを一つでも覆す可能性を残さない。要求K件に不足する場合、
遅い未知候補が不足分を埋める可能性も無視しない。
未知区間を含む下界探索自身がWork/メモリで完走しなければ認証しない。
Graph収録の失敗、Query Work/ラベル/乗換/メモリ上限もCompleteへ戻さない。
加重待ち時間でコリドー外の周回が有利になり得る場合は従来どおりUnknown。

実際の停車時には、既存のVehicleID・発着occurrence・施設・乗降条件と署名を
再検証する。署名はスキップした暗黙指令も含めて全raw ordersをハッシュする。
暗黙指令の自動追加・削除でindexがずれれば古いJourneyは失効する。
必要なら再探索またはOD-aware Fallbackに移行し、中間駅で最終ODへの配送を
成立させない。予約・部分積載・Gradual Loadingは既存の実座席割当を使用する。

## 計算量と再試行

暗黙停車の全体早期終了を単に外して名目予測を最後まで走らせる変更にはしていない。
静的コリドー選択を維持し、暗黙のみの車両を対象ODから除外する。
未知時刻でscoped予測を止めるので、未知の一周分を毎回生成しない。
fleetのcoverageを一度集計し、各停車駅から全fleetを繰り返し走査する処理も除去した。
構造インデックスは再利用するが、時刻・位置・容量・遅延・Dispatchを次のイベントで
取り直す。失敗した動的Graphを長期間キャッシュする変更はない。
同一停車イベントのOD結果共有と既存の再試行/公平なtick予算を維持する。

未知下界探索はサービスを一度だけ展開し、確定した下流occurrenceを状態ごとに
一度だけ展開する。発駅・状態からのdeparture走査、heap/map、改善回数の
メモリ・Workも同じquery予算に計上する。密な接続と多数のODが残る場合は
従来の16 snapshots/tick上限に達し得る。再構築回数の削減を保証する変更ではない。

| 予算 | 維持した値 |
|---|---:|
| Graph Work / memory | 2,097,152 / 128 MiB |
| total / per-trip visits | 262,144 / 4,096 |
| Query Work / labels / scratch | 65,536 / 8,192 / 8 MiB |
| tick graph / query-and-signature Work | 16 Mi / 1 Mi |
| tick snapshots | 16 |

冷/温cacheで同じ論理予算を払い、順序はpool IDとシミュレーションtickに依存する。
実時間、GUI、allocator状態、cacheの物理的滞在で判断を変えない。
新しいsave fieldはない。

## 回帰検証

MSVC x64 Release、assertions有効。旅客選択 `[passenger]` は**191 cases /
135,563 assertions**通過。CTestは最終的に**408/408 entries**通過した。
最初の実行では404件が通過し、4件は回帰データ未コピーで失敗した。
`regression_files` を構築した後、WindowsフォントAPIを使用できる同じ環境で
残る4件を再実行して全通過した。これはデータ/実行環境の修復で、判定コードを
緩めた結果ではない。

* 無関係/コリドー内/最速になり得る列車の暗黙履歴、phantom乗換の排除、raw index維持。
* 暗黙でなくTimingが停止原因となるケース、一部fleet/区間のみ完全、早い/遅い未知下界、
  同点とK不足、一般化費用が到着時刻の順位を逆転する反例。
* Conditional、Depot、Slot、Counter、RefitのUnknownと指令変更後の回復。
  車両進行・時刻表更新、1,001 tripsの未知dwell早期終了。
* 15通りの部分予測を完全ネットワーク予測と比較し、認証した6結果は全て基準結果と一致。
* 認証した不完全Tripの物理区間の適用・予約・部分積載・実乗車。
  予測外停車と暗黙指令挿入後の失効、再計画したraw alighting index、最終OD配送。
* 全国規模の循環/並行/直通、普通/特急、高頻度/低頻度、四交通モード乗継、
  8,001駅の本番コンテキスト、Work/メモリ上限と冷/温cache再生の一致。
* 既存のsave/load、OD生成、CargoDist無効時の同一性、GUI/診断/シャドーのsave hash・
  RNG不変性、予約失効、誤配送防止を含む本番runnerも通過。

ライブの複数peer/server連続運転試験は実施していない。マルチプレイについては
固定予算・cache再生一致・RNG/save不変性で検証した範囲に限る。

## 実マップ再測定

前回のverified-result由来の同じinput.sav（SHA256
`14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`）から、
修正前・修正後をそれぞれ新規プロセスで直列に実行した。元の入力コピーもハッシュ不変。
Passenger CargoDist automatic、OD assignment/Journey routing有効、recalc interval 8 /
time 32。区間はstate tick `3176545487 → 3176547014`、scaled tick
`3010946389 → 3010947916`、calendar `117528-03-11 → 117528-04-01`。
終了時には全15 LinkGraph jobsがcompleteだった。

今回の両側は `null:until_exit` video、null sound/music、同じassets/configを使用した。
既存のmissing/checksum NewGRF・運行中のcapacity変更警告は残る。
両測定専用バイナリには、コピー上でNewGRF警告の続行を選択することに相当する
`PauseMode::Error` 解除をconsole unpause時にだけ加えた。
基準側には同じStateGameLoop区間の時刻ログも加えた。
[測定専用差分](measurements/passenger-journey-scoped-uncertainty-2026-10-12/measurement-baseline-harness.patch)。
この差分は基準commitへのzero-context patch（`git apply --unidiff-zero`）。修正後の
測定ではconsole解除のhunkだけを加え、tick時間は製品側の診断カウンタを使った。
製品ソースには警告解除を残しておらず、復元後のソース差分ハッシュも一致を確認し再構築した。
`null` の通常の1,000-loop停止を避け、次calendar monthのconsole scriptがstats/save/quitする。
比較できない途中停止・準備runはこの表に含めない。

前回はwin32 videoだったため、前回の248.36秒と今回の修正後を直接CPU比較しない。
今回の基準側は前回と**構築数・Opaque/Work/Memory数・予測停止数・全Graph Workが一致**し、
検索0・適用0を再現した。下表の時間比較は今回の同条件ペアを使う。
[修正前console](measurements/passenger-journey-scoped-uncertainty-2026-10-12/before-console.txt)、
[修正後console](measurements/passenger-journey-scoped-uncertainty-2026-10-12/after-console.txt)。

| 1,527 state ticksの観測 | 今回の修正前 | 修正後 |
|---|---:|---:|
| ServiceGraph構築 | 24,369 | 23,939 |
| Opaque失敗 | 23,507 | **0** |
| Graph Work / Memory失敗 | 862 / 0 | 782 / 0 |
| 失敗なしの候補収録（未知Tripを含む） | 0 | 23,157 |
| Graph Complete（GetCoverageで時刻完全） | 0 | 278 |
| Planner検索 | 0 | **23,157** |
| OD単位の不確実性解消 | 0 | **1,708 searches** |
| イベントOD結果cache hits | 0 | 11,587 |
| Journey適用 | 0 packets / 0人 | **1,580 packets / 3,850人** |
| Journey実乗車 | 0人 | **1,659人** |
| Journey実乗換 | 0人 | **0人** |
| Journey最終OD配送 | 0人 | **604人** |
| 終了時Journey在庫 | 0人 / 0 packets | 1,067人 / 338 packets |
| Graph構築累計 | 293.346秒 | 472.144秒 |
| Graph構築平均 / 最大 | 12.037 / 29.808 ms | 19.722 / 43.410 ms |
| Planner検索累計 | 0秒 | 51.715秒 |
| 最大tick Graph+search | 295.929 ms | 519.068 ms |
| 完了simulation tick平均 / 最大 | 241.818 / 375.992 ms | 402.220 / 614.933 ms |
| Graph論理Work合計 | 13,697,260,849 | 13,671,037,171 |
| Graph平均 / 最大Work | 562,077 / 588,818 | 571,078 / 626,957 |
| Query admission allowance合計 | 0 | 1,522,851,232 |
| 保持visits合計（構築ごとの累計） | 696,078,843 | 346,534,836 |
| Graph保持推定の最大 | 5,491,614 bytes | 9,577,508 bytes |
| Graph+index論理envelope最大 | 29.149 MiB | 37.017 MiB |
| Query scratch最大 | 0 bytes | 1,828,419 bytes |
| プロセスpeak working set（ロード含む） | 2,015,232,000 bytes | 2,013,106,176 bytes |
| プロセスCPU / wall（ロード・保存含む） | 488.344 / 390.544秒 | 750.734 / 628.089秒 |
| 駅待機人数：開始 → 終了 | 200,450 → 203,731 | 200,450 → 203,008 |
| 終了時deferred人数 | 48,452 | 49,173 |

State tick差は1,527だが、終了scriptは最終tickのIncreaseDate内で実行される。
vehicle処理を終えたフルtick時刻は双方**1,526 samples**で比較した。
最終部分tickのstats/save/quit時間は含めない。時刻はsteady_clockによる経過時間。
[基準側tick CSV](measurements/passenger-journey-scoped-uncertainty-2026-10-12/before-ticks.csv)。
Query allowanceは固定のadmission debitで、実際に走査したquery Work合計とは異なる。
適用数は再探索による再適用を含み得るpacket-count合計で、人の一意識別件数ではない。
実乗車・乗換・配送もJourneyモードの移動イベントに含まれる人数である。
OD解消もunique OD数ではなく検索件数。

不確実性診断はTiming **19,262,659 trips**、制御/経路/状態等 **3,122,723 trips**。
Trip停止内訳はTiming 19,262,659、Unsupported 2,717,692、VehicleState 310,640、
OrderProgress 94,386、Work 5。それ以外は0。
これらは構築ごとの累計観測であり、車両数ではない。
Timingの件数増加は、従来Opaqueで中断した後のfleetまで収録した影響もある。
暗黙停止・暗黙Opaqueは0。Opaqueを別の名前のCompleteへ置き換えたのではなく、
時刻未知を保持し、最適性証明を通ったODだけ適用した。

| Fallback理由 | 修正前 packets / deferred | 修正後 packets / deferred |
|---|---:|---:|
| GraphWork | 0 / 1,086 | 0 / 967 |
| GraphMemory | 0 / 0 | 0 / 0 |
| SnapshotLimit | 0 / 362,323 | 0 / 67,879 |
| QueryWork | 0 / 21,439 | 5,050 / 320,086 |
| QueryLimit | 0 / 0 | 2,252 / 0 |
| QueryMemory | 0 / 0 | 0 / 0 |
| PredictionIncomplete | 34,161 / 0 | 24,883 / 0 |
| PredictionHorizon | 0 / 0 | 0 / 0 |
| NoJourney / InvalidLeg | 0 / 0 | 0 / 0 |
| StaleLeg | 0 / 0 | 772 / 0 |
| Disabled / VehicleDeleted | 0 / 0 | 0 / 0 |

Fallback数は各処理・再試行のpacketイベント数で、在庫packetsとは異なる。
実運行のずれに対してStaleLegが772件生じ、誤った継続ではなく再評価へ移行した。
この測定ではJourney乗換の成功は確認できず、実マップのmultimodal Journey実乗換を
実証した結果ではない。四モードの乗継は回帰fixtureで検証している。

## 利用できるようになったケースと残る制約

暗黙だけの無関係車両があるネットワーク、暗黙履歴を持つ明示指令の速い列車、
目的地到着後に初めて時刻が未知になるTrip、既知アクセス時間/確定境界のため
未知のどの候補も一般化費用を改善できないODが利用可能になった。
実マップでも適用・実乗車・最終OD配送を確認できたため、
「安全に確定できるODが全く存在しない」という状態ではなかった。

それでも多数のODはFallbackのまま。例えばtick `3176545489` のOD **120 → 115** は
確定候補を1件、score **1,112 ticks**で見つけたが、Service **4** を使う未知緩和の
到着下界が**その同じtick**だった。最速となる可能性を排除できずUnknownにした。
OD **43 → 823** ではService **1** を含む未知到着下界が `3176547968`
（readyから2,480 ticks）でも、目的地までの確定候補がなくUnknown。
OD **43 → 3871** は4,096 horizonで候補なし、延長した検索がQuery Workを尽くした。
下界は方向等を楽観的に緩和するので、これらは実運行で必ず速い未知便があるという
主張ではなく、現状の保証情報では競合を除外できないという診断である。

Graph構築数は**1.8%減**、visitsは**50.2%減**だが、構築時間は**61.0%増**、
フルsimulation tick平均は**66.3%増**。従来Opaqueで処理しなかった残りのfleetと
departure/component indexを完成させ、23,157回の検索・証明を実行した負荷がある。
メモリ上限は引き上げていない。無条件に早期終了を除去する代わりに、
scoped予測を最初の未知時刻で打ち切る抑制を入れたが、CPU削減を達成した結果ではない。
全国の密なblock（最大643 services）が残り、Graphがほぼ毎tick上限まで再構築される
制約はまだ大きい。長期の失敗cacheで動的回復を隠す対処はしていない。
今後の性能課題はイベント内の追加共有、より強い保証下界、予測/検索の段階的処理であり、
今回の達成を低負荷化や全OD利用可能化と読み替えない。
