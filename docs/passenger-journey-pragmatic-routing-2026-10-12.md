# Passenger Journey の時刻付き優先・未知便 Fallback（2026-10-12）

作業ブランチは `feat/passenger-journey-pragmatic-routing`、ベースは
`fix/passenger-journey-scoped-uncertainty` の `eab176fa0a`。

## 実装

時刻付きの最終OD到達候補を採用するための `BoundUnknownJourneys()` を削除した。
未知便の架空の下界や所要時間を作らず、時刻付き候補の従来の評価・容量・乗換・
車両/出現順の比較を維持する。`PassengerJourneyResult::GetOutcome()` は経路採用を
予測完全性から分離する。未知便のあるGraphの結果は Unknown のままでも
TimedRouteFound にできる。Work、memory、label、transfer ceiling は BudgetDeferred。

時刻付き候補が利用できない時だけ、既存の PassengerOdFallbackContext を使う。
現在の列車による直通を優先し、それ以外は unrestricted LinkGraph の目的地からの
逆向きホップ距離を使って、実際の指令接頭部にあるすべての進行可能な駅を検討する。
残りホップ数、StationID の順で決定する。接続情報は進行のヒントであり、将来の
接続車両や空席の保証ではない。毎回の乗車でライブ指令・停車設備・乗降制約を検証する。
距離が減らない乗車、元の駅への循環、強制乗換直後の同じ車両への再積載を拒否する。
時刻なし便は既存 passenger_via で実行し、PassengerJourneyExecution に時刻を保存しない。

距離配列と車両/OD判断（失敗を含む）を停止イベント内で共有する。全国Graphと
未知経路の検索をパケットごとに実行しない。距離配列1 MiB、逆隣接scratch2 MiB、
128 entries、128 Ki event work、4 Mi tick work に制限する。ライブ設備検証も課金する。
暗黙停車の予測方針、セーブ拡張、CargoPacketサイズ、RNGは変更していない。

## 診断とGUI

既存の Journey applied/transport、Graph/Query work/time、Fallback理由を残した。
新たに timed_ignoring_unknown、timed_budget_deferred、untimed、untimed_no_route、
untimed_budget_deferred、Untimed transport、方式別待機人数を出力する。
`untimed` は停止イベントで採用したライブ車両/cargo/OD判断であり、人数や予約成立数ではない。
実輸送は実際のCargoLoad、予約から実積載への移動、CargoTransfer、CargoDeliveryだけを数える。
繰り返し移動や再計画も数え、ユニーク旅客数とは区別する。
GUIは Timed Journey / CargoDist or deferred / OD Fallback or waiting とし、
Fallback全員の経路確保を主張しない。廃止した最適性証明の診断値は0/なしとして残す。

## 検証

既存の旅客テストに加え、時刻付き普通列車対未知特急、時刻なし直通便の予約・部分積載、
混合経路の実乗換・最終配送、別の最短経路の停車駅選択、同一OD 4,096回の判断共有、
古い乗換駅ノードの失効からの回復を検証する。時刻付きだけの参照ネットワークとの比較を
複数の未知便待ち時間で行い、時刻付き選択の不変性と下界探索の廃止を確認する。
既存テストは車種別乗降制約、満員/容量、部分積載、予約失効、指令/暗黙停車変更、
再試行、公平な予算、Cold/Warm cache、RNG、セーブ・ロードと診断/駅GUIのセーブ純粋性を含む。
ライブのマルチプレイ長時間運転と描画された駅GUIの目視確認は行っていない。

## 実マップ測定

前回と同じ入力SHA-256 `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`。
初期State ticks 3,176,545,487 から最終 3,176,547,014 までの1,527 ticksを順番に測る。
最後のpartial tickは集計・保存が車両処理より前なので、tick timingは1,526 samples。
MSVC19.44 x64 Release、asserts ON、LTO OFF、null/null/null drivers、
PAX distribution 2、OD/Journey routing ON、recalc 8/32。NewGRFの継続操作だけを
測定バイナリに追加し、製品ソースへ残さない。既存のNewGRF容量変更警告は両方に発生する。
セーブとアセットは隔離コピーで実行し、元のセーブを変更しない。

両方の測定は正常終了し、State ticksと1,526 tick timing samplesを確認した。
基準測定は前回と同じ3,850人の適用・1,659人の乗車・604人の配送を再現した。
ただし前回の平均402msに対し今回の基準は482msであり、時間は環境の変動を受ける。
下表は今回の順次測定同士の比較で、単一ペアの測定値である。

| 指標 | 既存仕様の再測定 | 変更後 |
|---|---:|---:|
| 平均 simulation tick | 482.194 ms | 348.720 ms（27.7%減） |
| 最大 simulation tick | 2,081.713 ms | 655.385 ms |
| 全tick処理時間 | 735.829 s | 532.147 s |
| ServiceGraph構築時間 | 560.279 s | 408.304 s |
| ServiceGraph構築回数 | 23,939 | 23,464 |
| Planner検索時間 | 62.553 s | 9.347 s（85.1%減） |
| Planner検索回数 | 23,157 | 22,809 |
| 停止イベントOD再利用 | 11,587 | 12,131 |
| Journey適用人数（再計画含む） | 3,850 | 22,414 |
| 時刻付き経路の適用packet数 | 1,580 | 9,461 |
| 不完全予測を無視した時刻付き適用 | 未計測 | 9,050 packets |
| 時刻なしライブ経路採用 | 未計測 | 23,892 decisions |
| 時刻付き実乗車 | 1,659 | 7,673 |
| 時刻付き実乗換 | 0 | 1 |
| 時刻付き最終配送 | 604 | 1,731 |
| 時刻なし実乗車 | 未計測 | 81,242 |
| 時刻なし実乗換 | 未計測 | 39,405 |
| 時刻なし最終配送 | 未計測 | 31,078 |
| 最終総待機人数 | 203,008 | 211,069 |
| 最終Journey保有人数（待機+車上） | 1,067 | 6,007 |
| 最終時刻付きJourney待機 | 未計測 | 2,606 |
| 最終deferred人数 | 49,173 | 51,460 |
| プロセスpeak working set | 1,933.2 MiB | 1,914.3 MiB |
| サンプリングpeak private bytes | 1,935.5 MiB | 1,915.4 MiB |
| プロセスCPU時間 | 821.969 s | 624.234 s |
| プロセスwall時間（ロード/保存含む） | 749.041 s | 546.505 s |
| Opaque Graph構築 | 0 | 0 |

旧バイナリは時刻なし輸送を独立計数していないので、時刻なし・全体配送の増減は
この比較から主張できない。変更後の時刻なし最終配送31,078人は実CargoDeliveryで
確認した人数であり、適用数だけの成功判定ではない。時刻付き乗車は4.62倍、
時刻付き配送は2.87倍になった。旅客単位の追跡・平均旅行時間は測っていない。

| 予算・Fallback観測 | 既存仕様 | 変更後 |
|---|---:|---:|
| GraphWork deferred packets | 967 | 788 |
| SnapshotLimit deferred packets | 67,879 | 9,461 |
| QueryWork fallback / deferred | 5,050 / 320,086 | 2,332 / 440,416 |
| QueryLimit fallback packets | 2,252 | 2,586 |
| PredictionIncomplete fallback packets | 24,883 | 20,312 |
| 時刻付きbudget観測（新規） | 未計測 | 455,583 |
| 時刻なし経路なし判断 | 未計測 | 32,219 |
| 時刻なしbudget判断 | 未計測 | 637,053 |
| OD Fallback探索回数 | 126,954 | 76,409 |
| OD Fallback work | 859,217,725 | 2,509,180,773 |

予算表は再試行を含む観測であり、ユニーク旅客数ではない。Timed budgetは
Graph/Snapshot/Queryの資源上限で拒否したpacket観測の合計である。
個別理由の全件は生consoleとmetadataに保存した。

## 残る問題

ServiceGraph構築は変更後も408秒、全tick時間の76.7%を占める。
23,464回の構築に対して検索は9.35秒で、未知便の最適性証明を取り除いた後の
主な負荷はODごとの動的車両・指令・時刻予測の再構築に残る。
構造indexの再利用は維持したが、異なるOD間の動的予測共有は今回再設計していない。
構築時間の減少をすべてこの変更に帰属させることはできない。

総待機は基準より8,061人（4.0%）多く、待機改善は確認できなかった。
時刻なし探索の回数は減った一方、逆距離探索は目的地側の接続を広く走査するため
workが約2.92倍で、637,053件の予算制限判断が残る。固定の時刻付きquery admission
でもQueryWork deferredが増えている。予算下での安全な輸送・再試行は行えるが、
全ODを速やかに処理できたとは言えない。目的地ごとに繰り返す逆隣接処理や、
同じイベントでの動的予測の共有は今後の改善候補である。

NoLoad/NoUnload、強制乗換、設備、実容量、予約失効、OD保持を緩和して輸送数を
増やしてはいない。時刻なし経路は将来の運行・空席を予約しないので、
古い接続情報や路線変更で途中待機する場合がある。
既存NewGRFの容量変更警告は両測定に発生した。実際のマルチプレイ長時間運転と
描画GUIの目視は未実施で、決定性は自動テストと保存状態比較の範囲で確認した。

## 変更ファイル

- `src/passenger_journey_planner.cpp/.h`: 未知便の下界探索を削除、経路結果と完全性を分離。
- `src/passenger_journey_active.cpp/.h`: 時刻付き採用、ライブFallback、候補/キャッシュ/予算と方式別診断。
- `src/cargoaction.cpp`、`src/cargopacket.cpp`: 実際の乗車・乗換・配送を方式別に計数。
- `src/console_cmds.cpp`、`src/lang/english.txt`: 採用/実輸送/待機診断とGUI/設定の表示更新。
- `src/passenger_service_graph.h`: capture完全性と時刻付き経路の比較の説明更新。
- `src/tests/passenger_journey_active.cpp`、`passenger_journey_planner.cpp`、`passenger_service_graph.cpp`: 新方針・予算・経路候補・保存・実配送の回帰検証。
- `docs/passenger-journey-active.md`、`passenger-journey-planner.md`、`passenger-service-graph-scaling.md`: 現行方針を文書化。
- 本レポートと測定ディレクトリ: 条件・hash・生の結果・検証出力。

最終旅客テストは **196 cases / 139,658 assertions**、全CTestは **413/413**。
測定専用操作を取り除いて製品を再生成した後も、production save/loadテストを確認した。


[測定メタデータ](measurements/passenger-journey-pragmatic-routing-2026-10-12/metadata.json)
と生のconsole/process/test出力を保存する。大きなセーブ・NewGRF・バイナリは
`build-pragmatic-routing/run/` に置き、Gitへ追加しない。
