# Periodic Journey Query Work Admission

最新ベース `7753377108` への追従と統合検証は[2026-10-15のレポート](passenger-query-work-admission-integration-2026-10-15.md)を参照。旧ベースでの修正後の再測定・実v4セーブは[2026-10-14の最終修正レポート](passenger-query-work-admission-final-2026-10-14.md)に保持する。以下の初期性能測定は履歴として保持する。

ベースは `8a96044d40`（Agent A統合後の `jgrpp-ya`）。Agent Bの路線探索はこのベースに未統合であり、ユーザー確認により現在の `jgrpp-ya` から実装した。ブランチは `feat/passenger-query-work-admission`。

レビュー後、診断用Work履歴がPacket結合を禁止していた実装を撤去した。以下の実マップ性能・配送値は撤去前の測定であり、修正版の性能実績としては扱わない。旧 `intent_wait_ge_4096` はTimedの未設定時刻を含むため無効で、長時間待機の根拠から除外した。原本は保持し、[レビュー訂正記録](measurements/passenger-query-work-admission-2026-10-14/review-corrections.json)で区別する。

## 受付と精算

Snapshot用 `PassengerJourneyActiveContext::AdmitQuery()` は従来のまま。Periodicだけが `PassengerQueryAdmission` を使い、Globalの1,048,576 Work/tickとイベントの4,194,304 Work残量から最大65,536 Workを予約する。少ない残量でも部分予約できる。既存のイベント128検索上限に加え、Periodicには固定16 OD/tickの上限を設けた。

予約はコピー不可のスコープ付きオブジェクト。Timed/Schedule-awareが返す論理Workを各1回 `Charge()` し、スコープ終了で未使用分を両予算へ戻す。消費した検索件数は戻さない。途中return・例外の巻き戻しでも予約は解放される。キャッシュの実計算省略を課金へ使わず、各plannerのcoldと同じ論理Workを使う。

初期配分はTimed通常枠49,152、Schedule-aware保護枠16,384。TimedのCertified/Provisionalな完全経路を優先し、利用できる経路がない場合だけSchedule-awareを実行する。Schedule-awareにはTimedの全未使用Workを貸し出す。部分予約時は保護割合を維持する。保護枠をTimedへ貸し出して再探索する方式は採用していない。

Schedule-awareは既存のEstimatedServiceIntentを返す。待機時間は頻度からの推定であり、全乗換の絶対時刻が接続する完全Timed経路・最適性の証明として表示しない。

## 決定性とfair share

CPU時間・実時間・キャッシュ状態は受付や停止の入力にしない。件数・Workはsimulation tick境界で初期化し、診断resetで予算を戻さない。Save/Load・途中参加の境界も既存の `BeginPassengerJourneyTick()` を使う。

既存のdue loading census、stationローテーション、128 KiWorkの予約、未使用shareの解放をPeriodicにも適用する。他stationの未取得shareごとに2件の検索枠も保護する。待機再試行間隔とstation cohortは変更しない。同じイベントのcargo/destination/ready/excluded-serviceに対する結果と受付拒否を共有する。

公開済み索引のOriginMissing/DestinationMissingはTimedが1 Workで検出するので、Schedule-awareの重複検索を省く。DBが構築中なら未公開としてdeferし、公開済み世代なら通常のfallback/cooldownを使う。どちらも永久に到達不能とは保存しない。更新後の再試行は残すが、存在しないODへの全国探索を繰り返さない。

## 診断

`passenger_journey_stats total|tick` に受付成功・拒否理由、予約・実使用・返却、Timed/Schedule配分・消費、実検索件数/Workのtickヒストグラムと最大を追加した。`reserved = used + returned`、`used = timed_used + schedule_used` を検証できる。

`stopped` はTimedの最終ReasonがWorkのOD数。`incomplete_work` はその時点でTimed経路がないOD数。`no_route` はSchedule-awareを含めた後も経路が得られなかったOD数。率の分母は受付済みOD数。同一ODのイベント内共有は検索として重複計上しない。完全経路あり・最適性未証明は `complete_unproved`、Work後のSchedule救済は `schedule_rescued` として別に出す。

再試行はcargo/origin/destinationの4,096エントリ以内のローカル観測で、累積再検索数・同一OD最大試行数・16標本・追跡eviction数を出す。eviction後の回数は下限であり、ゲームの再試行制御には使わない。

Work履歴によるPacketフラグ・結合条件・保存フィールド・長期追跡カウンタは撤去した。Work経験旅客だけの乗車・配送人数は現在は未計測で、0人として報告しない。Timed/Schedule/Untimedの実乗車・乗換・最終配送と、Work終了・両経路なし・Schedule救済のOD集計は保持する。長期追跡はAdmissionから独立した測定方式で設計し直す必要がある。

新規保存は既存の実行状態だけを持つ `passenger_journey_routing` v3へ戻した。読込はv4まで許可し、既存v4の廃止済み診断フィールドは名前付きtableの未知フィールドとして読み飛ばす。true/falseの異なる旧履歴から読んだ同一経路のPacketも結合できる。

`inventory` の `schedule_intent_age_ge_4096` はSchedule-awareだけを数える。Timedの `planned_at` は未設定なので待機年齢は未計測と表示する。intentの経過時間は再計画時にリセットされ、全旅客の連続待機時間ではない。`transit_age_ge_256` は旅行時間を含むcargo年齢の代理指標であり、待機時間と混同しない。旧 `intent_wait_ge_4096` と新しいSchedule専用指標は比較できない。

## 回帰と再測定

Admission回帰で、同一ODの100 Packet共有、軽いODの固定件数上限、Global/Event境界、fair shareのWorkと件数保護、途中終了・例外時返却、両探索で65,536 Work消費、Timed49,152 Work後のSchedule救済、cold/warm、v3保存、splitと実移動観測を検証する。レビュー修正では旧v4のtrue/false履歴を含むtableの読込・Packet結合・履歴なし再保存と、Timed/Snapshot除外・Schedule-awareの4,096境界・未来時刻・再計画後の年齢を追加検証する。撤去前の旅客回帰は366ケース・2,726,174アサーションが成功した。

比較用 [Build-Variants.py](measurements/passenger-query-work-admission-2026-10-14/Build-Variants.py) は探索実装のコピーだけで32/64件、Timed49,152/32,768を作る。通常製品に実験設定やローカル判断フラグは追加しない。[Run-Comparison.py](measurements/passenger-query-work-admission-2026-10-14/Run-Comparison.py) は固定実マップを共有測定ロック・競合監視の下で逐次実行し、StateTicksの実進行を確認する。

Agent Bは同じleaseの `TimedAllowance()` を路線plannerのlimits.workへ渡し、返された論理Workを `Charge()` すれば、同じFallback保護と精算を利用できる。路線探索の導入や性能合格をこのブランチの実績として扱わない。

採用値は16 OD/tick、Timed49,152 / Schedule保護16,384 Work。32/64件と低Timed枠のCPU増加を実測してから、16/24件も追加比較した。検索数の拡大は今回の製品では見送る。

## 初期実マップ比較と採用判断

同じ16384×4096マップ、固定入力SHA-256 `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`、各8,192 simulation ticks。MSVC 19.44 x64 Release、assert ON、LTO OFF、静的vcpkg、null video/sound/music。共有測定ロック、90秒の競合なし、実行中5秒ごとの競合監視を使用した。3件の競合試行は[除外記録](measurements/passenger-query-work-admission-2026-10-14/excluded-runs.json)に残し、表から除外した。

| 固定OD上限 / Timed枠 | simulation平均 ms/tick | p95 / p99上端 ms | Timed最終配送 | 全carrier最終配送 | Work不足で両経路なし / 受付OD |
|---|---:|---:|---:|---:|---:|
| 対照：旧65,536一括課金 | 32.405 | 51 / 66 | 17,876 | 241,896 | 38,817 / 131,072 (29.61%) |
| 32 / 49,152 | 45.161 | 68 / 79 | 52,807 | 242,837 | 71,065 / 249,276 (28.51%) |
| 64 / 49,152 | 51.652 | 73 / 83 | 56,116 | 243,981 | 86,691 / 306,185 (28.31%) |
| 32 / 32,768 | 46.223 | 69 / 80 | 47,103 | 246,332 | 83,534 / 240,943 (34.67%) |
| 24 / 49,152 | 38.593 | 60 / 73 | 46,149 | 244,175 | 52,854 / 195,248 (27.07%) |
| **16 / 49,152** | **32.065** | **49 / 58** | **38,361** | **241,223** | **27,412 / 130,715 (20.97%)** |

これは各条件1ランの初期比較で、変更だけの因果的CPU改善率ではない。24/32/64件はCPU平均が19%/39%/59%増に対し、全配送の増加が0.94%/0.39%/0.86%に留まり、採用しなかった。Timed32,768も平均43%増で、両経路なし率が悪化した。

撤去前の16件案では、Work終了率25.39%と両経路なし率20.97%を分けて計測した。完全経路あり・最適性未証明は5,780 OD、Timed Work後のSchedule救済は11 OD。Work経験ODの延べ実乗車19,379人・最終配送201人は廃止した追跡方式の旧観測であり、結合制限のない修正版の移動人数とは解釈しない。

Timed実乗車108,529 / 乗換43,511 / 配送38,361、Schedule 223 / 53 / 130、Untimed 494,102 / 248,582 / 202,732人。全配送は対照比0.28%減でほぼ横ばいであり、配送総数の増加を成果として主張しない。実使用3,338,090,464 Work、未使用返却5,228,447,776 Work。観測最大は16 OD/tick・1,040,512 Work/tickで、Global上限を超えなかった。

プロセスCPUは対照366.750秒、16件案367.016秒。peak Working Setは2,087,579,648 / 2,087,997,440 bytes、sampled peak privateは2,085,257,216 / 2,086,563,840 bytes。ロード・保存・workerを含む値で、常駐メモリの厳密測定ではない。CargoPacketは192 bytesのまま。キャッシュ見積もりと実プロセス測定を混同しない。

初期比較の対照は旧active.cpp、その他の製品ソースとv4保存記述子は共通。新しいWork観測タグは新方式だけで付けるため、タグによるPacket mergeの差も含む全変更の比較であり、Admissionだけの効果へ分解していない。新方式はfair shareにより問い合わせ列自体も変わる。同一OD列の検索品質保証ではない。

初期バイナリの実装元は `77f465d9f6`。採用後に `f519df10ab` で固定上限16と、cached拒否の理由・FairWork/FairQueries区別を反映した。採用版の再測定・cache不変性・継続・途中参加を別に保存する。

## 採用版の再測定とcache不変性

`f519df10ab` の固定バイナリによる8,192 tick再測定は、対照32.709 / 採用版32.369 ms/tick。採用版のp95は49 ms以下、p99は59 ms以下。検索130,715、予約8,566,538,240 / 実使用3,338,090,464 / 返却5,228,447,776 Work、各carrierの実輸送、Work不足の結果は初期16件案と一致した。

Timed/Schedule共有policy・Departure Index・resident fallback cacheを無効にした同じ区間では、平均48.410 ms/tick。論理Work・受付と拒否・停止理由・再試行観測・実乗車/乗換/配送・待機censusが一致し、**最終Save全体のSHA-256も一致**した。SHA-256は `2fa1b07407db3ca3077664fb9aab21a13378d2df6d478c80d4cf81302495f1c2`。[実ファイル再hashによる一致確認](measurements/passenger-query-work-admission-2026-10-14/final-invariance.json)を保存した。イベント内共有とscratch再利用はこのcold条件でも維持し、scratch無効は既存C++回帰で検証する。

## 保存後の継続・途中参加・製品版

採用版の8,192 tick後のSaveから、さらに8,192 tick進めた。実StateTicksは `3176545487 → 3176553679 → 3176561871`、合計16,384 tick。[継続clock](measurements/passenger-query-work-admission-2026-10-14/final-continuation-clock.json)はdriver loops 118,486,489と実simulation ticks 8,192を区別し、Error pause解除は0回だった。継続driverは実tickが進むまで回すため、この区間のプロセスCPUを初期比較へ混ぜない。

| 追加8,192 tick | 結果 |
|---|---:|
| 受付OD / 同一OD再検索観測 / 最大試行数 | 130,854 / 11,803 / 125 |
| Work終了 / 両経路なし | 38,537 / 33,063 |
| 完全経路あり・未証明 / Schedule救済 | 5,478 / 9 |
| Timed 実乗車 / 乗換 / 最終配送 | 110,658 / 66,133 / 45,764 |
| Schedule 実乗車 / 乗換 / 最終配送 | 212 / 82 / 164 |
| Untimed 実乗車 / 乗換 / 最終配送 | 628,632 / 354,681 / 257,130 |
| Work経験OD 延べ乗車 / 最終配送 | 38,841 / 1,059 |
| 最大検索 / 最大実Work per tick | 16 / 983,902 |
| 実Work / 未使用返却Work | 3,658,261,987 / 4,917,385,757 |

最終待機は334,754人、そのうちknown OD待機282,642人。cargo年齢256以上の待機は11人。旧 `intent_wait_ge_4096=9503` はTimedの未設定時刻から数えた可能性があり、長時間待機人数の根拠として撤回する。新規生成を含むcensusなので、これを開始時の同一旅客集団の未達率とはしない。継続区間の全配送は303,058人で、対応する対照継続を追加測定したとは主張しない。[10ランの旧集計](measurements/passenger-query-work-admission-2026-10-14/summary.json)にはレビュー前の待機・未配送OD census・CPU/メモリ・停止・各carrier輸送・検証済みSaveハッシュを保存した。

loopback途中参加では初回と後発の2クライアントが最後まで接続し、serverは1,149 tick進んだ。policy・Departure・fallback・scratchを無効にしたクライアントでも、sync frequency 1で同期エラー・assertion・切断を検出しなかった。最終観測はserverと同tick / 39 tick以内（観測分解能128 tick）。serverはProvisionalを1,093 packet・延べ1,211人に採用した。[途中参加結果](measurements/passenger-query-work-admission-2026-10-14/final-multiplayer-result.json)とserver/clientのconsole原本を保存した。

最後に計測TUを外して `OPTION_PASSENGER_BENCHMARK=OFF`、assert ON、LTO OFFのMSVC x64 Releaseをビルドし、[586テストが全件成功](measurements/passenger-query-work-admission-2026-10-14/final-ctest.log)。旅客回帰は366ケース・2,726,125アサーション、測定集計検証5件も成功した。[製品版のバイナリ・compiler/library確認](measurements/passenger-query-work-admission-2026-10-14/final-product-verification.json)を保存した。

継続補助metadataの重複SHA欄には親バイナリのSHAが残っていた。実行バイナリのSHAは主要manifestとprocess記録で再hash検証し、原本を保持して[訂正記録](measurements/passenger-query-work-admission-2026-10-14/measurement-errata.json)を残した。再実行用 `Final-Checks.py` は修正済み。経路・Work・時間標本・Saveデータには影響しない。

再測定は構成済みbuildと固定packageに対して `Build-Variants.py`、`Run-Comparison.py` を使う。`Final-Checks.py BUILD PACKAGE DEPENDENCIES` は最終warm測定のsequenceを待ち、cold・継続・途中参加・計測なし製品版を共有ロックの下で逐次実行する。`Verify-Invariance.py WARM_MANIFEST COLD_MANIFEST --output RESULT.json` は実Saveを再hashする。Save・NewGRF・exeはGitに含めない。

## レビュー修正の検証

計測なしのMSVC x64 Release製品版と専用テストtargetを再ビルドし、[588件の全回帰](measurements/passenger-query-work-admission-2026-10-14/review-fixes-ctest.log)と診断集計の8件が成功した。CargoPacketの実装・結合条件はベース `8a96044d40` と一致する。旧v4で履歴true/falseを保存したtableを現在の記述子で読み、同一経路Packetを結合してv3で履歴なし再保存する回帰も通った。Timed/Snapshotの計画時刻0を除外し、Schedule-awareの時刻0・4,096境界・未来時刻・再計画後の年齢と読取時の状態不変性を確認した。

[検証記録](measurements/passenger-query-work-admission-2026-10-14/review-verification.json)には製品・テストバイナリと全回帰ログのSHA-256を保存した。今回は修正版の長時間実マップ性能を再測定していないため、前掲の旧CPU・配送・メモリ値を修正版の性能保証へ転用しない。
