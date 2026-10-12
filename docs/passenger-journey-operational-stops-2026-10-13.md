# 旅客経路の運転停車と車庫指令

2026年10月13日、`sjcl/feat/passenger-journey-periodic-backend` の最新HEAD
`4bf419c4bf4dd4b0610b4ef403411c33013ace1f` から
`fix/passenger-journey-operational-stops` を作成した。
旅客の乗降候補と物理的な運行イベントを分離し、周期方式と旧Snapshot方式、
ライブ乗車検証、Untimed Fallbackで同じ車庫分類を使用する。

## 指令の扱い

| 指令 | 旅客経路 | 時刻計算 |
|---|---|---|
| 通常の駅停車 | 乗車・降車可能 | 明示されたTravelとWaitを使用 |
| 乗車のみ | 乗車可能、降車不可 | 同上 |
| 降車のみ | 降車可能、乗車不可 | 同上 |
| NoLoadとNoUnloadの運転停車 | 当該CargoTypeの乗降・乗換候補から除外 | TravelとWait、周期、物理Stopは保持 |
| Waypoint、ブイ、折り返しWaypoint | 乗降・乗換不可 | Travelと設定されたWaitを保持 |
| 通常の指令入庫 | 乗客を保持して後続駅へ継続可能 | 固定車庫、PartOfOrders、Refit・Halt・Sellなし、TravelとWaitの明示フラグが必要 |
| 必要時整備、最寄り車庫、Unbunching | 既知の後続駅へのライブ到達可能性を使用 | 経由や出発時間を推測せずUntimed |
| Halt、Sell、Refit | 車庫越しの継続乗車を拒否 | 周期全体はUntimed |
| 自動入庫やBreakdown指令 | 指令列の継続を保証しない | 周期全体はUntimed |

途中の強制Unload・Transferは継続乗車を遮断する。最終ODで受入可能なら
Transfer-onlyでも配送を完了する既存例外は維持する。
Full Loadや早発許可は予測上無視し、実際の車両制御は変更しない。
未知の将来の暗黙停車や条件分岐も予測しない。

明示的な0と未設定時間は区別する。駅と車庫の未設定Waitは0でも時刻不明となる。
WaypointのWaitフラグがなく値も0の場合は、既存の通過指令として待機イベントがない。
明示的なWaypoint Waitと折り返しフラグは維持し、未設定のTravel 0や正値の未設定Waitを
時刻既知へ昇格させない。車庫にDispatch基準を割り当てた場合はUntimedのままとする。

## 参照と保存の維持

周期Stopを削除せず、元のOrderID、Stop index、発着オフセット、車両位相、
Dispatch基準、ServiceIntentとBoundServiceのoccurrenceを維持する。
駅別索引は乗降マスクの和集合を保持し、ODの接続成分探索と候補生成ではCargoTypeごとの
boardとalightを確認する。運転停車の時間は同じ列車に乗り続ける区間へ含める。

旧SnapshotのStopとVisitも物理イベントとして保持する。
乗降不能なVisitは駅別Visit・Departure・接続成分の索引から除外する一方、
onboardの現在Visit検索には残す。`GetVisitStop()` の参照関係を変更しない。
Scoped索引のキーにCargoTypeを追加し、実際の乗降・Non-stop指令変更で派生索引を失効させる。
時刻だけの変更は既存の1024tick epochで公開する。

JPSDのwire versionは1のまま。保存された世代・位相・更新キューを再構築せず復元する。
`AfterLoadPassengerJourneys()` と駅待機キューのロード処理は変更していない。
以前の実装が車庫を理由にUntimedとして保存したレコードも、その公開状態を維持する。
通常の無効化・再公開が発生するまで、ロードだけで新しいTimedレコードへ置き換えない。

## 回帰検証

MSVC 19.44、Release、assertions有効、LTO無効で検証した。
製品構成の`OPTION_PASSENGER_BENCHMARK=OFF`でCTest **483件すべて成功**。
追加した7件の`[operational]`テストは**593 assertionsすべて成功**した。

追加ケースは、運転停車の30ticks、待機時間編集とepoch再公開、百万周回の周期演算、
最初の運転停車をDispatch基準とする場合、2旅客CargoTypeの異なる乗降制約、
乗車のみ・降車のみ・運転停車の混在、Waypointの時間フラグと折り返し、車庫の各分類、
強制乗換、Snapshotとの発着比較、運転停車中のonboard継続、保存・復元一致を検査する。
ModifyOrderコマンドで乗降制約を変更し、駅名と車両IDが同じでも古いScoped索引を
使わないことも確認した。
乗車駅と同じStationIDへNoUnloadの運転停車として戻るケースでも、周期方式、
旧予測、Untimed Fallbackの継続乗車が一致する。

Train、RoadVehicle、Ship、Aircraftの各アダプターについて、両バックエンドと必要時整備を
組み合わせ、25人のうち20人を予約して5人ずつ積載し、運転停車で保持した後、
Transfer-onlyの最終ODで20人を配送する。既存の削除、予約返却、Refit、RNG、
Cold/Warm Work、旧セーブ、GUI・診断の保存純度テストも通過した。

初回の製品ロード試験3件は実行ファイル側の言語資産が不足していた。
`Release/lang`と`Release/baseset`を配置して解消し、最終全回帰で再確認した。

[全回帰ログ](measurements/passenger-journey-operational-stops-2026-10-13/ctest-final.log)、
[追加テストログ](measurements/passenger-journey-operational-stops-2026-10-13/operational-tests.log)。

## 実マップの測定

前の13.6ms測定を比較基準には再利用していない。
最新ベースと最終変更バイナリを、同じ隔離セーブ・資産・設定で各2回、逐次実行した。
入力SHA256は`14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`。
地図は16384×4096、初期待機人数は200450人。
state ticksは`3176545487→3176547014`で、完了tick標本は各1526個。
Asymmetric、OD割当・周期Journey有効、`recalc_interval=8`、`recalc_time=32`。
測定中に他のビルド、テスト、ゲームを重ねていない。

測定用オプションはONとして既存のNewGRF容量変更警告によるError pauseを解除した。
ベースの測定専用fixtureにあったClientPoolIDとClientIDの比較誤りを、`client_id`を使う
1行修正で直した。経路アルゴリズムはベースのまま。ログ用複製はPEのconsole指定だけを変更した。

単位はms。p95とp99は1msヒストグラムの上限値。

| バイナリと反復 | 全体平均 | p95 | p99 | 最大 | Journey平均 | p95 | p99 | 最大 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| ベース1 | 38.859 | 53 | 58 | 87.340 | 14.181 | 26 | 29 | 39.572 |
| ベース2 | 33.449 | 54 | 60 | 76.420 | 11.834 | 26 | 30 | 37.566 |
| 最終変更1 | 34.683 | 51 | 59 | 83.147 | 12.110 | 25 | 29 | 57.251 |
| 最終変更2 | 40.325 | 57 | 66 | 87.519 | 14.338 | 28 | 32 | 46.804 |

2回平均は全体36.154→37.504ms（+3.7%）、Journey13.008→13.224ms（+1.7%）。
同じバイナリ内のばらつきも大きい。速度改善や性能回帰なしを断定する測定ではない。

時間以外の以下の結果は、各バイナリの2回で一致した。

| 指標 | 最新ベース | 最終変更 |
|---|---:|---:|
| 末尾公開系統数とTimed系統数 | 837 / 509 | 837 / 509 |
| DB公開レコード構築数 | 5413 | 3188 |
| DB構築Work | 19775343 | 12015015 |
| DB構築時間の2回平均 | 262283us | 292548us |
| 周期OD検索数 | 24416 | 24416 |
| 周期OD検索時間の2回平均 | 14.490秒 | 14.454秒 |
| ポリシーヒットとミス | 7440 / 16976 | 9298 / 15118 |
| ポリシーヒット率 | 30.5% | 38.1% |
| 全体無効化回数 | 5 | 2 |
| 末尾Pending | 558 | 559 |
| DB論理メモリ | 8498064byte | 8498064byte |
| 時刻付きJourney採用 | 4016 packets | 3821 packets |
| Untimedライブ経路採用 | 24533 | 24324 |
| Timed 実乗車と乗換と配送人数 | 2458 / 0 / 534 | 2818 / 0 / 779 |
| Untimed 実乗車と乗換と配送人数 | 86582 / 39236 / 31822 | 85033 / 39029 / 31568 |
| 配送合計 | 32356人 | 32347人 |

DB構築Workは約39%減少し、全体無効化は5→2回となった。候補探索と実際の経路・積載の
変更を含む実運転の結果であり、単独の最適化だけへ帰属させる測定ではない。
末尾公開系統数・Timed系統数・車両数・DB byteはベースと一致した。
プロセスpeak working setはベース1902.6～1906.1MiB、変更後1905.8～1909.4MiB。
100ms samplingで、ゲーム資産等も含む。

変更後の末尾公開DBでは旅客CargoType別の物理occurrenceが9825、乗降可能occurrenceが9807。
18個の運転停車候補を除外した。車庫を含む公開系統は196個で、いずれもUntimed分類、
Timed分類は0個だった。このマップはTimed車庫の性能検証例にはならない。
車庫対応だけによる経路採用数を、他の経路選択変更と切り分けて帰属させる計測はしていない。
ServiceGraph構築と物理Trip予測は引き続き0回。

[測定カウンター](measurements/passenger-journey-operational-stops-2026-10-13/summary.json)、
[比較とソース識別](measurements/passenger-journey-operational-stops-2026-10-13/comparison.json)、
[再測定用スクリプト](measurements/passenger-journey-operational-stops-2026-10-13/Measure-Operational.ps1)、
[集計スクリプト](measurements/passenger-journey-operational-stops-2026-10-13/Analyze-Measurements.py)。

## 保存済みJourneyを含む途中参加

製品構成の同じ実行コードを、PEのconsole指定だけ変更した複製で検証した。
実マップ測定の最終セーブにはJPSDとJourney旅客が保存されている。
そのセーブを私設loopbackサーバーでロードし、Routing設定を再適用せず保存値を維持した。
初期クライアントと途中参加クライアントを接続し、`network.sync_freq=1`、CPU制限なしで
途中参加後90秒を観察した。

state ticksは`3176547014→3176549808`の2794tick進行。
途中参加前にJourney旅客3994人、末尾に4535人が存在した。
両クライアントは終了確認時も接続しており、同期ACKは初期36回、途中参加32回。
同期不一致、desync report、タイムアウトはいずれも0件。
時刻付き旅客の乗車4813人・配送3202人、Untimedの乗車192128人・配送78104人を記録した。
有限のローカル試験であり、WANや長期運用全体の保証ではない。

[同期結果](measurements/passenger-journey-operational-stops-2026-10-13/mp-result.json)、
[サーバーログ](measurements/passenger-journey-operational-stops-2026-10-13/mp-server-console.txt)、
[検証バイナリ識別](measurements/passenger-journey-operational-stops-2026-10-13/verification-metadata.json)、
[途中参加ハーネス](measurements/passenger-journey-operational-stops-2026-10-13/Exercise-Multiplayer.py)。
再実行は`Exercise-Multiplayer.py <package> <saved-game> <output> --binary openttd-console.exe --backend preserve --duration 90 --net-debug 7`。

## 変更ファイル

* `src/passenger_routing.{h,cpp}`：物理停車・貨物別乗降判定、車庫分類、Snapshot予測の車庫時間処理。
* `src/passenger_service_database.{h,cpp}`：CargoType別の接続成分と候補、車庫を含む周期、ライブ検証、診断。
* `src/passenger_service_graph.{h,cpp}`：物理StopとVisitを維持し、乗降不能な駅別索引を除外。
* `src/passenger_service_index.{h,cpp}`：CargoTypeを含むScoped索引と貨物別乗降候補。
* `src/passenger_journey_active.cpp`：継続可能な車庫の先をUntimed Fallbackで探索。
* `src/order_cmd.cpp`：乗降指令変更時の派生索引失効。実車制御は変更しない。
* `src/console_cmds.cpp`：運行イベントの診断と、測定専用fixtureのID型修正。
* `src/tests/passenger_service_database.cpp`、`passenger_multimodal.cpp`、`passenger_service_graph.cpp`、`passenger_service_graph_index.cpp`：回帰と参照索引を更新。
* `docs/passenger-journey-periodic.md`、本報告、測定ディレクトリ：仕様・生ログ・再実行手順。

## 残る制限

未対応の条件分岐、Refit、車庫Dispatch、周期内の一部区間だけのTimed化は引き続き対象外。
必要時整備やUnbunchingに架空の発着時刻を付けない。
実車の経路・施設・容量が変化した場合は既存のライブ検証と無効化処理に従う。
小規模のTimed車庫比較と全車種の貨物処理テストは、全車種が物理車庫を走行する長期実運行の証明ではない。
全国規模のDB無効化とODヒット率の差について、追加の原因調査余地がある。
