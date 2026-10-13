# OD FallbackのLinkGraph Bindingキャッシュ（2026-10-13）

ベースは `jgrpp-ya` の `a58d472382f85c63fe9114888d71d7d189c45572`、
作業ブランチは `perf/passenger-fallback-bindings`。
`src/passenger_service_database.cpp/.h` は変更しない。

## 全Node検証とWork

従来の `PassengerOdFallbackContext::Distance()` は、イベント内のBindingが未登録なら、
resident Bindingにヒットしても全NodeのStationIDと `valid_node()` の結果を比較していた。
`valid_node()` はStation Poolの存在、貨物別 `GoodsEntry::link_graph`、`node` を読む。
距離配列やReverseIndexがヒットしても、この比較を先に実行する。
同一イベントではBindingと距離配列を共有しているため、packetごとに必ず走査する処理ではない。

`fallback_distance_us` はDistance全体の包含時間。
`validation_ns` はresident Bindingの全Node比較ループのみ、`build_ns` はBinding再生成と収録、
`reverse_build_ns` はReverseIndex生成と収録である。包含時間同士は加算しない。
短い呼出しでの切捨てを避けるため、追加した内部スコープはnanosecondsで集計する。
診断の時計、hit率、物理走査数を経路判断・予算・保存状態へ使用しない。

比較ループとBinding生成の中には `Spend()` がない。最初の `Spend(graph->Size())`、
edge数のprefix課金、BFSのNode・edge課金、ライブ指令と施設の課金を変更していない。
距離配列のhitでは従来どおりCold BFSのWorkを再現し、予算不足なら同じ失敗prefixを消費する。

## Revision監査

保存される `PassengerOdRevision()` は、通常のNode追加・削除・統合とedge追加・削除を捕捉する。
しかしStation Poolの一括初期化、GoodsEntry単独の対応変更、ロード時に戻る保存Revisionは、
それだけではBindingの同一性を保証しない。保存Revisionを増やすとODの保存状態・ジョブの検証にも
影響するため、以下の派生専用世代を追加した。

Bindingのキーは `LinkGraphID / PassengerBindingRevision / PassengerStationBindingGeneration / Size / CargoType`。
両世代はNOSAVE、simulation thread専用、Pool cleanup・新規ゲーム・Loadでリセットしない。
Node表は外部から変更できず、更新入口は以下のとおりである。

| 操作 | 検出方法 |
|---|---|
| Graph生成・LinkGraphID再利用 | コンストラクタが新しいBinding Revisionを取得 |
| Node追加 | `AddNode()` がOD RevisionとBinding Revisionを更新 |
| Node削除・末尾Nodeの交換 | `RemoveNode()` が両Revisionを更新。交換先GoodsEntryはsetter経由 |
| Graph統合 | 統合先の両Revisionを更新し、全GoodsEntryをsetterで付け替える。統合元は削除 |
| Graph分割 | このコードにはGraph分割処理がない。edge削除後もGraph内の有向到達可能性を再計算する |
| Station削除・StationID再利用 | Station構築・破棄で寿命世代を更新。通常削除は各CargoのNodeも削除 |
| Station Pool一括初期化 | `CleaningPool()` の早期returnより前に寿命世代を更新 |
| GoodsEntryのgraph/node変更 | `SetLinkGraph()` が変更前・変更後のGraphの派生Binding Revisionを更新 |
| Cargo別Graph割当 | 全実行時書込みをsetterへ集約。GraphのCargoは構築時固定、BindingキーにもCargoを含む |
| edge追加・削除 | 既存のOD Revisionで距離とReverseIndexを失効。Binding対応は再走査不要 |
| edgeの制限・開放・capacity有無 | 既存のRouting Revisionで距離とReverseIndexを失効 |
| Station施設変更 | 通常の施設フラグはNode対応の判定材料ではない。既存の実車prefix・施設検証で毎イベント判定。Station/Waypointの種別は構築・破棄・Loadで確定 |
| Save/Load・途中参加 | Graphの`Init()`で新しい派生Revision。Station構築で寿命世代を更新。AfterLoadでresident cacheを破棄 |
| immutable LinkGraphJob | simulation threadでGraphをcopy。workerはライブBinding世代を書き換えない |

`GoodsEntry::link_graph` と `node` のフィールドは既存のsave table互換を維持している。
実行時の書込みは `SetLinkGraph()` を使う必要がある。raw save/load tableは直接復元するが、
Graph Init、Station構築とAfterLoadのキャッシュ破棄で別の同一性になる。
現行の実行時書込みは `station_cmd.cpp` の `IncreaseStats()` / `UpdateStationWaiting()`、
`LinkGraph::Merge()` / `RemoveNode()` に集約されており、すべてsetterへ変更した。

`PassengerRoutingRevision()` とOD Revisionは距離とReverseIndexのキーに維持する。
Bindingのtokenは再生成ごとに更新し、古い距離配列が新しいNode有効性と組み合わされない。
Binding 32件/8MiB、ReverseIndex 32件/8MiB、距離512件/16MiBの上限は維持した。

## 挙動と検証入口

ODへの距離減少、有向到達可能性、StationIDによる同点選択、NoLoad/NoUnload、
実車prefix、車庫・運転停車、強制降車、予約・実乗車・最終配送の処理は変更していない。
origin/destinationのStationとNodeの対応はhit時もライブ検証する。

`passenger_journey_stats fallback_cache_off` はresident Binding・ReverseIndex・距離配列を無効にする。
イベント内共有と論理予算は維持する。`fallback_cache_on` で再度有効にできる。
`fallback_validate_on` は参照用にresident Bindingの物理全Node照合を有効にする。
これらはローカルの未保存設定で、Load後も維持するため、resident有効サーバーと無効クライアントの
途中参加を比較できる。

## 回帰テスト

MSVC 19.44 x64 Release /MT、assertions ON、LTO OFF。
製品構成 `OPTION_PASSENGER_BENCHMARK=OFF` のCTestは **544/544成功**、98.81秒。
全旅客テストは **331ケース・979,474 assertions成功**。
新しい `[bindings]` の10ケース・1,498 assertionsは、同一Graph反復、GoodsEntry単独変更、Station削除・再追加、
末尾Node交換、Graph統合、保存OD Revisionも同じGraphID再利用、Station Pool再初期化、
edge制限・開放・削除・新OD経路、施設変更、32件と512件の退避、save/loadを検査する。
列車・道路車両・船・航空機の各アダプターでCache ON/OFFの経路とWorkを比較し、
20人を実予約・5人ずつの積載・強制乗換・最終配送まで実行する。
既存のNoLoad/NoUnload、車庫・運転停車、同点選択、予算失敗、RNG、旧セーブ、GUI・診断の保存純度も通過した。

生ログは [CTest](measurements/passenger-fallback-bindings-2026-10-13/ctest-product.log)、
[全旅客](measurements/passenger-fallback-bindings-2026-10-13/passenger-tests.log)、
[Binding](measurements/passenger-fallback-bindings-2026-10-13/binding-tests.log)。

## 同一実マップの比較

入力SHA256は `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`。
16384×4096の地図、初期待機200450人、state ticks `3176545487→3176553679` の8192tick。
Asymmetric、OD・周期Journey有効、`recalc_interval=8`、`recalc_time=32`。
元セーブと資産は変更せず、同じ隔離コピー・資産・設定から各ランを開始した。
測定専用オプションONで、既存のNewGRF容量変更によるError pauseを解除する。
同一資産の容量変更警告は両バイナリにあり、完全にNewGRFの一致した環境の検証ではない。

最初のbaseline1はビルドと重なった探索計測なので速度比から除外する。
以下は当方のビルド・テストを終了した後の逐次ラン。別のOpenTTDプロセスは並行して動いていた。
単一ホストでの観測であり、Journey全体やsimulation全体の速度保証には使用しない。

| 指標 | baseline2 | optimized1 |
|---|---:|---:|
| Distance包含時間 | 28.836秒 / 3.520ms/tick | 20.084秒 / 2.452ms/tick |
| resident Binding照合だけ | 9.890秒 / 1.207ms/tick | 0秒 |
| 物理照合Node数 | 635246744 | 0 |
| Binding構築数 / 時間 | 4 / 0.200ms | 4 / 0.240ms |
| ReverseIndex構築数 / 時間 | 4 / 0.456ms | 4 / 0.449ms |
| Binding hit / miss | 134916 / 4 | 134916 / 4 |
| 距離hit / 検索 | 114041 / 309182 | 114041 / 309182 |
| 距離hit率 | 26.95% | 26.95% |
| Fallback論理Work | 14790064347 | 14790064347 |
| Journey平均 / p95 / p99 / 最大 | 24.551 / 42 / 65 / 150.521ms | 24.407 / 43 / 68 / 117.034ms |
| simulation平均 / p95 / p99 / 最大 | 48.712 / 69 / 92 / 184.942ms | 49.853 / 71 / 96 / 143.994ms |
| cache論理bytes | 16871860 | 16871860 |
| process peak working set | 2090831872 bytes | 2089836544 bytes |
| Untimed実乗車 / 乗換 / 配送人数 | 552990 / 274926 / 226209 | 552990 / 274926 / 226209 |

p95/p99は1msヒストグラムの上限値。Distanceは観測上30.35%減少し、未課金の全Node照合を除去した。
距離hit率は既存診断の `hits / (hits + searches)`。Work不足で失敗したresident再生はhit計数前に戻るため、
すべてのキャッシュlookupを分母にした率ではない。`searches` は物理BFSの試行数である。
Journey平均は約0.6%減、simulation平均は約2.3%増であり、全tickの改善は立証していない。
キャッシュ保持量は同じで、約1MiBのprocess peak差もメモリ削減とは主張しない。

baseline1、baseline2、optimized1の最終セーブはバイト一致し、SHA256は
`1bae3398586e1abfac6d305d9af825a2d26e76cbc69fc21369d2549f5a61c590`。
residentキャッシュOFFのcold1も同じ8192tickを完了し、Fallback Work、Timed・Untimedの輸送人数、
最終セーブのバイトとSHA256がすべて一致した。距離hitと保持bytesは0、Binding構築134920回、
ReverseIndex構築450576回。Distance包含時間は108.797秒、Binding生成12.252秒、
ReverseIndex生成69.911秒、Journey平均34.929ms、simulation平均59.798ms。
物理BFSの試行数は450576回となった。resident再生のWork不足は検索計数より前に戻るため、
ON時の `hits + searches` とOFF時の検索数は一致を要求しない。論理Workと保存状態は一致を要求する。
cold1のprocess peak working setは2084720640 bytesで、ロード・資産・保存も含む。

機械可読結果・生ログは [測定フォルダ](measurements/passenger-fallback-bindings-2026-10-13/summary.json)。
計測前のソース差分は [baselineの計測パッチ](measurements/passenger-fallback-bindings-2026-10-13/baseline-instrumentation.patch)、
ベースの隔離checkoutへ `git apply --unidiff-zero <patch>` で適用する。
元のcontext付き差分と同じ3ファイルを生成することを照合済み。
再実行は `Measure-Bindings.ps1 -RunDirectory <run> -Binary <exe> -Label <label> -Ticks 8192`。
OFF比較には `-DisableFallbackCache` を追加する。

## Save/Loadと途中参加

製品構成の実行コードを、PEのsubsystemだけconsoleへ変更したコピーで検証した。
入力は上記のバイト一致する最終セーブ `optimized1-result.sav`。
保存されたRouting、周期DB、Journey状態を維持して読み込み、サーバーはresident ON、
初期参加・途中参加の両クライアントはresident OFF。`network.sync_freq=1`、私設loopback
`127.0.0.1:17523`、途中参加後90秒を観察した。

state ticksは `3176553679→3176555349` の1670tick進行。
末尾チェックで両クライアントが接続し、初期参加19回・途中参加14回の同期ACKを確認。
同期不一致、desync reportは0件。Untimedの実乗車134100人・乗換75585人・配送56779人、
Timedの実乗車11339人・乗換6855人・配送5078人を記録した。

最初の2クライアントを同じ1コアへ制限した試験では接続が失われ、ACKは2回・1回のみだった。
同期不一致は観測しなかったが成功とは扱わない。
その[未確定結果](measurements/passenger-fallback-bindings-2026-10-13/mp-single-core-inconclusive.json)を残し、
CPU制限を外して最終再試験を行った。CPU制限付き運用、WAN、長期運用全体の保証ではない。

[同期結果](measurements/passenger-fallback-bindings-2026-10-13/mp-result.json)、
[サーバーログ](measurements/passenger-fallback-bindings-2026-10-13/mp-server-console.txt)、
[バイナリ・ソース識別](measurements/passenger-fallback-bindings-2026-10-13/verification-metadata.json)。
再実行は `Exercise-Multiplayer.py <run> <output> --binary product-console.exe --input-save optimized1-result.sav --backend preserve --duration 90`。
セーブ・資産・バイナリはリポジトリへ追加せず、識別値とテキスト証跡を保存した。
