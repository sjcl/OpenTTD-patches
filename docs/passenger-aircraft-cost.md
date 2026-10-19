# 新旅客経路探索の航空機コスト

既存の `linkgraph.aircraft_link_scale` と `linkgraph.aircraft_link_add` を旅客の評価コストにも適用する。設定項目の追加や CargoDist の計算変更はない。

## 計算と実時刻

航空機による隣接する実停車間ごとに、移動時間 `m` に対して次を計算する。

```
flight_cost = floor(m * aircraft_link_scale / 100)
            + aircraft_link_add * DAY_TICKS * DayLengthFactor()
adjustment = flight_cost - m
```

通常の一般化コストに `adjustment` を加える。駅での停車、乗車・乗換待ち、途中の waypoint/depot 待ち、容量・不確実性ペナルティには倍率を掛けない。途中で降りない A→B→C も二つの区間として丸め・固定加算する。列車、道路車両、船には差額を加えない。

実出発・到着時刻、時刻表の周期、運行頻度、容量、実移動距離・運賃用の距離は変更しない。予測訪問と周期サービスに差額の累積値を構築し、通常の照会は差を引くだけにする。乗車中の再計画では現在の訪問以後の差額だけを計上する。100%・0日では累積配列を確保しない。

`PassengerAircraftCost` は整数の商・余りで乗算し、最終値が収まらない場合だけ飽和する。割引は移動時間の置換なので、航空区間の評価コストは非負を維持する。

## 各方式

| 方式 | 適用箇所 |
| --- | --- |
| Timed | 訪問間の累積差額をラベルと完成経路に反映。割引時には実到着時刻による評価コスト下界を使わず、異なる時刻のラベルの支配を保守的に扱う。 |
| Periodic | サービスごとの累積差額を前向き評価、逆向き下界、サービス枝刈りに反映。支配は遅い側の時刻までの実待ちも含めて比較する。 |
| Schedule 推定 fallback | 評価だけに同じ差額を反映し、実乗車時間の診断値は保持する。 |
| Precomputed | グラフの Ride 辺と共有区間に適用。辺の評価値と実乗車時間を別に持ち、圧縮・参照エンジン双方で維持する。 |
| Hyperpath | 同じ Ride 辺に加え、候補の再評価、期待コスト、再訪問の支配・継続証明も同じ評価値を使う。 |
| Untimed | 既存 CargoDist の計算を使う。加算・割増された航空機の直行便が、有効な CargoDist の別経由駅を無視して乗車されることを防ぐ。 |

非デフォルト設定では、航空機と非航空機の同一停車パターンを統合しない。共有区間の頻度統合もこの区別を保持する。既存の Precomputed の近傍代替選択、Hyperpath の頻度に基づく候補集合は維持するため、Timed と常に同一の一便を選ぶわけではない。

評価値が予測 horizon を超える経路も、範囲内に実到着すれば採用できる。割引でより遠い時刻の候補が有利になる場合、Active の horizon 探索は既存の最大範囲まで延長する。既存の探索予算・最大 horizon 自体は変更しない。

## 設定変更・保存・非同期処理

両設定の post callback は `PassengerAircraftCostSettingsChanged` を呼ぶ。運行情報・位相を再収集せず、差額累積値と任意の経路キャッシュを更新し、現行・保持中の事前計算表を破棄する。旧ワーカー・協調構築も破棄してから新しい入力を作り、待機経路を無効化する。day length の変更は既存設定通知および tick のコスト比較で検出する。

事前計算の開始時に倍率・換算済み固定加算を値で固定する。グラフ構築・ワーカーは可変のグローバル設定を読まない。保存には現行／保持中の各設定スナップショットも含め、ロード・途中参加では保存済み Work を既存の決定的な公開手順で再現する。ローカルの worker 数・完了時刻は公開結果を変えない。

JPSD 内部形式と `passenger_periodic` feature を2に更新した。内部形式1も読み込むが、移動／途中待ちの分離情報がないため一度だけ運行情報から再構築する。この移行により、デフォルトで旧セーブをロードして後から設定を変えた場合も、途中待ちを誤って拡大しない。旧バイナリは新形式に対応しない。

## 検証範囲と制限

関連テストは `[aircraft_cost]`。割増・割引、区間ごとの加算、day length、他モードとの混在、途中待ち、乗車中の残区間、極端な整数値、下界と支配、大きな加算と Active horizon、同一パターン、設定変更、旧形式移行、構築途中の保存再生、異なる worker タイミングを確認する。

2026-10-20、Windows x64 / Clang 23.1.3 / Release（assert有効、PGO・LTO無効）で本体と `openttd_test` のビルドに成功。最終 CTest は **800/800成功**、関連14ケースは **1,234 assertions成功**。[検証記録](passenger-aircraft-cost-validation-2026-10-20.txt) に実行コマンドと要約を記録した。

Untimed は CargoDist の駅単位の経由情報に依存する。同じ駅間で航空機と鉄道を区別できない既存の制約や、CargoDist の通常の再計算間隔は残る。新たなパケット単位の重み付き探索は追加していない。実マップの長時間ベンチマークは実施しない。

## 変更ファイル

| ファイル（`src/` 内） | 内容 |
| --- | --- |
| `passenger_aircraft_cost.h` | 固定した設定値、day length 換算、オーバーフローしない整数演算。 |
| `passenger_routing.cpp`, `passenger_routing.h` | 予測訪問に移動だけの時間を記録。 |
| `passenger_service_graph.cpp`, `passenger_service_graph.h` | Timed の差額累積値とメモリ診断。 |
| `passenger_journey_planner.cpp` | 評価、割引時の下界・支配、残区間だけの再計画。 |
| `passenger_service_database.cpp`, `passenger_service_database.h` | 周期サービスの累積値、下界・支配・枝刈り、設定変更、表の破棄、保存再生。 |
| `passenger_journey_schedule.cpp` | 推定 Schedule の評価。 |
| `passenger_precomputed.cpp`, `passenger_precomputed.h` | グラフ・共有区間・Hyperpath と実乗車時間の分離。 |
| `passenger_journey_active.cpp`, `passenger_journey_active.h` | 設定 callback、Active horizon、untimed 航空機の CargoDist 経由維持。 |
| `table/settings/linkgraph_settings.ini` | 既存二設定への callback 接続。 |
| `sl/extended_ver_sl.cpp` | 保存 feature のバージョン更新。 |
| `tests/passenger_multimodal.cpp`, `tests/passenger_precomputed.cpp`, `tests/passenger_service_database.cpp` | 挙動・境界値・設定変更・非同期・保存の回帰テスト。 |
| `tests/test_main.cpp` | ゲームの設定ローダーを実行しない単体 fixture に100%・0日の既存デフォルトを用意。 |
| `CMakeLists.txt` | 新ヘッダーをソース一覧に登録。 |
