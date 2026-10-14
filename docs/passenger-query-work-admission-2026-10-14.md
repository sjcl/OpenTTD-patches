# Periodic Journey Query Work Admission

ベースは `8a96044d40`（Agent A統合後の `jgrpp-ya`）。Agent Bの路線探索はこのベースに未統合であり、ユーザー確認により現在の `jgrpp-ya` から実装した。ブランチは `feat/passenger-query-work-admission`。

## 受付と精算

Snapshot用 `PassengerJourneyActiveContext::AdmitQuery()` は従来のまま。Periodicだけが `PassengerQueryAdmission` を使い、Globalの1,048,576 Work/tickとイベントの4,194,304 Work残量から最大65,536 Workを予約する。少ない残量でも部分予約できる。既存のイベント128検索上限に加え、Periodicには固定32 OD/tickの上限を設けた。

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

Packetの `passenger_work_exhausted` はTimedがWork終了したODの観測フラグ。split・fallback・乗換・保存を通じて保持し、実Cargo移動のBoard/Deliverだけを数える。乗車は乗換で再び数えるため延べ人数。最終配送は全carrierを含む。このフラグは保存拡張 `passenger_journey_routing` v4で保存し、v3以前はfalse。フラグの異なるPacketは観測属性を失わないようmergeしない。

`inventory` の待機人数と `intent_wait_ge_4096` も測定する。intentの経過時間は再計画時にリセットされ、全旅客の連続待機時間ではない。`transit_age_ge_256` は旅行時間を含むcargo年齢の代理指標であり、待機時間と混同しない。

## 回帰と再測定

6件のAdmission回帰で、同一ODの100 Packet共有、軽いODの32件上限、Global/Event境界、fair shareのWorkと件数保護、途中終了・例外時返却、両探索で65,536 Work消費、Timed49,152 Work後のSchedule救済、cold/warm、旧v3/新v4保存、splitと実移動観測を検証する。既存の旅客回帰は366ケース・2,726,174アサーションが成功した。

比較用 [Build-Variants.py](measurements/passenger-query-work-admission-2026-10-14/Build-Variants.py) は探索実装のコピーだけで32/64件、Timed49,152/32,768を作る。通常製品に実験設定やローカル判断フラグは追加しない。[Run-Comparison.py](measurements/passenger-query-work-admission-2026-10-14/Run-Comparison.py) は固定実マップを共有測定ロック・競合監視の下で逐次実行し、StateTicksの実進行を確認する。

Agent Bは同じleaseの `TimedAllowance()` を路線plannerのlimits.workへ渡し、返された論理Workを `Charge()` すれば、同じFallback保護と精算を利用できる。路線探索の導入や性能合格をこのブランチの実績として扱わない。

実マップのCPU・輸送比較と採用値は測定後に追記する。
