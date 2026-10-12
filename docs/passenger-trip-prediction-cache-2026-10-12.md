# Passenger Trip予測のイベント内再利用 — 2026-10-12

`jgrpp-ya` の `1556b330cd9360d1e2f6b0ad50e45b70b86c6341` をベースに、
`perf/passenger-trip-prediction-cache` へ実装した。同じ入力の物理Trip予測を
再利用する。pragmatic routingの選択・Fallback・乗降・保存形式は変更していない。
実マップの予測実行は約60%減ったが、全tick時間の改善は確認できなかった。

所有者は既存の `PassengerJourneyActiveContext` 内の `PassengerServiceCapture`。
BeginLoadingまたは待機列の同期的な経路探索イベントが終わると破棄される。
別イベントは同じtickでも別所有者を作る。外部からBuildを直接呼ぶ場合も、
イベントの開始時や呼出側によるゲーム状態変更前に `Clear()` が必要。
tickまたは構造index revisionの変化でも全キャッシュを破棄する。

検索表はVehicleID・CargoTypeで候補を探すだけで、ヒット判定には次の値をすべて
照合する。ハッシュの一致だけでは再利用を認めない。表の列挙順は使わない。

| 入力 | 照合方法 |
|---|---|
| 物理車両・指令一覧・貨物 | VehicleID、OrderListID、CargoType、車種、先頭車のCargoType |
| 時間・探索条件 | start、end、最大Visit数、最大Step数、prefix_onlyの完全一致 |
| 現在の運行状態 | real/implicit/timetable order index、elapsed、lateness、loading、dispatch有効状態、operation |
| 位置・方向 | tile、x/y/z、向き、後退を反映した運行方向 |
| 積載・共有運行 | 実容量、実積載人数、共有車両数、現在のOrderの所有値 |
| 指令・時刻表・Refit | type、flags、dest、refit cargo、travel/wait/max speed、extra flags/data、全貨物の積卸 flags、dispatch index |
| Scheduled Dispatch | start、duration、last dispatch、delay、reuse、全slotのoffset/flags/route ID |
| 停車施設 | 毎回GetPassengerRouteStateを実行し、全Orderのusable_stops判定結果を比較 |

指令・Dispatchは所有するimmutable ServicePatternとライブ値を比較する。
変更があれば前処理とTripキャッシュをまとめて破棄する。表示名・GUI色・Orderの
occupancy表示値は予測に使われない。実積載人数は別途毎回取得し、照合している。
共有Patternの所有ポインタは、この値検証を通った後のidentityとして使う。
車両・Station・OrderListへの生ポインタは保持しない。

Discoveryにも全一覧・共有車両・編成部分のID、貨物、容量、Refit・停止判定を
所有値で記録して照合する。以前は除外していた貨物車のRefit、新しい共有車両、
旅客容量を変えない編成増減も見落とさない。編成増減はDiscoveryのWorkも変える。
これらの検証の経過時間は診断だけに使い、元の論理Workを再現する。

保存するTrip結果はCompleteまたは資源打切りを伴わないUnknownだけ。
Work・Memory・Visits・Stepsで途中終了した結果は保存しない。
元の予測Workを支払え、全Visitの320 bytesずつの予約を支払える場合だけ再利用する。
ヒットでも元のWorkを差し引き、同じVisit予約とadmitted_visitsの更新を行う。
不足する場合は共通のPredictPassengerVisitsを実行し、Workとメモリ失敗の順序、
first failure、停止cursor、complete_untilまで元通りに取得する。
Visit配列は順序・到着/出発・loading_now・leg duration・不確実性・停止理由を丸ごと
コピーする。Trip/VisitIDは従来通りグラフのcapture順から生成する。

期間が異なる結果は再利用しない。4096→16384→65536も、長期間から短期間への
変更も再計算する。境界を切り出したり、短い結果を長い完全予測として扱ったりしない。
継続計算や別の時刻予測エンジンは追加していない。

Tripの保持枠は最大8 MiBで、既存Capture全体の16 MiB枠にも含める。
Visit/usable_stopsのコピーと交換、入力・コンテナの固定費を含む保守的な保持
envelopeで、容量観測やRSSをadmissionに使わない。VehicleID/CargoType当たり
期間・制限variantは1件まで。満杯なら格納を断り、通常予測へ戻る。
Trip枠を0にすると前処理は共有したままTrip再利用だけを無効にできる。
論理Graph/Eventメモリ上限とWork上限は変更しない。

`passenger_journey_stats tick/total` は物理実行回数、ヒット数、予測Work、物理生成
Visit数、予測時間、Trip保持枠のピークを出力する。従来のpredictedとvisitsは
ヒット分を含む論理数。各Tripにも実行/ヒット・Work・時間を保持する。
`passenger_service_capture_benchmark 0 0 100` は同一イベントの100 ODを比較し、
車両/CargoType/startごとの出現数・実行数・ヒット数・Visit数・Work・時間を出力する。
`repeated_vehicle_cargo_start / requests` が粗い重複率で、入力の完全一致率とは別。
ここでの実行回数は物理TripのPredictPassengerVisits呼出し数で、既存のnominal
ServicePattern予測やboarding検証の呼出しは含まない。両側で前処理を共有する。

最終製品のCTestは **426/426、22.40秒**。旅客の非ベンチマーク選択は
**188 cases / 876,408 assertions**、Trip/Capture選択は **10 cases / 53,851 assertions**。
明示的な性能fixtureは **2 cases / 4,899 assertions**。製品のproduction save/load、
診断・GUIの保存純粋性、shadow比較、AI/GameScript回帰も含む。

| 要求テスト | 検証内容 |
|---|---|
| 1–2: 同じ列車を100 OD、同じイベント | 同じ列車は実行1回・ヒット99回。全Journey/Workも比較 |
| 3: 別イベント | Clear後と次tickは新規予測。同一tickの新イベントも独立 |
| 4–5: 位置・遅延 | tile、同tile内座標、方向、後退、elapsed、latenessの変化でmiss |
| 6: 時刻表・共有指令・Dispatch | travel/wait、未記入時間、history/start/duration/delay/reuse、slot全属性の変更 |
| 7: Refit | 異なる旅客CargoType、混合編成、除外貨物車のRefit、実貨物変更 |
| 8: 施設・積載条件 | NoLoad/NoUnload、他貨物のfull load、実乗車人数、全車種の施設変更 |
| 9: 期間延長 | 4096→16384→65536→16384→4096。各段階の通常予測と境界を比較 |
| 10: 不完全時刻表・予算 | Work 0–399、メモリ64-byte刻み、Step/Visit/total Visit制限。停止詳細も比較 |
| 11: 多数の共有車両 | 同じOrderListの129物理Trip。別車両の結果を混同しない |
| 12: 非列車 | 実Train/RoadVehicle/Ship/Aircraft adapterを使用 |
| 13: 無効/有効一致 | Graph/Query/Plannerの結果・予算、CargoPacket実行保存bytes、RNG、tick allowanceが一致 |
| 14: 同期 | 無効・有効・極小容量のpeer相当テストと実マップ全保存bytesが一致。ライブ接続/join/soakは未実施 |

性能はMSVC 19.44、x64 Release、asserts ON、LTO OFFで測定した。時間は3回の中央値。
合成fixtureは4,800駅、2,399物理車両、100種類のOD、4,096-tick horizon。
ペアの実行順を交互にし、全経路と論理予算を比較している。

| 合成100 OD | Trip無効 | Trip有効 |
|---|---:|---:|
| 物理Trip予測実行 | 239,900 | 2,399 |
| ヒット率 | 0% | 99% |
| Graph構築時間 | 1.364563 s | 1.374824 s |
| 構築回数 | 100 | 100 |
| 論理Visit数 | 959,600 | 959,600 |
| Journey検索/時刻付き経路数 | 100 / 100 | 100 / 100 |
| 論理Graph Work | 26,869,300 | 26,869,300 |
| Trip追加保持envelope | 0 | 3,704,056 B |

128 KiB枠でも実行231,584・ヒット8,316となり、無制限に保持せず通常予測に戻る。
同規模のActiveContext tick内routing fixtureは8構築・8検索・適用0人で両側一致。
既存Query allowanceでこの密なネットワークを処理しきれないためで、これを
旅客輸送の改善とは評価しない。小さなpeer実行fixtureと実マップは適用成功も比較する。

実マップは既存のverified-result.savを隔離コピーし、通常NewGRF scanningを行った。
122個のGRF/tar資産を使用。不足GRF・削除された無効車両は0。
既存のJP+容量変更警告が発生するため、一時null-driver harnessでError/Normal
pauseを継続する。この操作は両側に同じように適用し、最終製品から削除した。
各プロセスは32 simulation ticksを完了した。第1ペアでは両側を同じ読み取り専用
100 OD測定で前処理し、後のペアはその測定を行わず開始する。

| 実マップ32 ticks | Trip無効 | Trip有効 |
|---|---:|---:|
| 物理Trip予測実行 | 1,484,995 | 599,161 |
| ヒット率 | 0% | 59.65% |
| 物理生成Visit数 | 6,920,998 | 2,792,819 |
| 論理Visit数 | 6,920,998 | 6,920,998 |
| Graph構築時間 | 6.243056 s | 6.349460 s |
| 構築回数 / Journey検索数 | 498 / 480 | 498 / 480 |
| Journey適用人数（再計画含む） | 470 | 470 |
| 論理Graph / Query Work | 284,684,466 / 32,042,568 | 同左 |
| 論理予測Work | 8,007,403 | 8,007,403 |
| Trip追加保持ピーク | 0 | 5,312,024 B |
| simulation tick総時間 | 9.045642 s | 9.290838 s |
| 平均simulation tick | 282.676 ms | 290.338 ms |

物理予測を59.65%削減したが、Graph時間は中央値で1.7%、tick総時間は2.7%増えた。
ペア別tick時間は-0.4%、+2.7%、+17.1%と変動しており、短い3組からゲーム全体の
速度向上は主張しない。小さい予測のコピー・格納・照合も費用になる。車両/施設の
ライブ検証とDepartureIndexは引き続き実行する。RSSや長時間運転は測っていない。

実マップの固定100 OD測定は、予測実行280,439→219,015、ヒット61,424（21.90%）、
粗い重複率98.88%、不一致0だった。Trip枠は8,387,594 Bでほぼ満杯であり、
重複率をそのままヒット率にできない。論理Work 70,407,374、Graph bound
79,085,888 B、最大Visit数178,068は一致。性能測定は旅客を移動しない。

入力SHA-256は `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`。
State ticksは3,176,545,487→3,176,545,519。6つの最終保存はすべて
`9e396742c4f4f49f4f3263564f7284a0d19f68a73b4778c6b886005b7bff88db`。
ライブマルチプレイの接続・join・長期運転まで検証したという意味ではない。

再現用の製品テスト・合成測定は次の通り。Windows回帰テストには生成された
lang/basesetとmedia/basesetのTTFを配置し、通常のWindowsフォントAPIを利用する。

```powershell
cmake --build build-trip-cache --config Release --target openttd_test openttd regression_files --parallel 8
ctest --test-dir build-trip-cache -C Release --output-on-failure -j 4
& ./build-trip-cache/Release/openttd_test.exe '[capture]~[.performance],[trip_cache]~[.performance]' --reporter compact
& ./build-trip-cache/Release/openttd_test.exe 'Trip prediction benchmark*,Trip cache synthetic tick*' --reporter compact > benchmarks.log
python utils/passenger_trip_prediction_benchmark.py benchmarks.log measurement-output
```

[全測定CSV](measurements/passenger-trip-prediction-cache-2026-10-12/runs.csv)、
[集計と予算・保存一致検証](measurements/passenger-trip-prediction-cache-2026-10-12/summary.json)、
[環境・binary hash・検証メタデータ](measurements/passenger-trip-prediction-cache-2026-10-12/metadata.json)、
[全CTest出力](measurements/passenger-trip-prediction-cache-2026-10-12/ctest-final.log)を保存した。
実マップ用の一時harness差分とrunner、各プロセスのconsole/stderr/stdoutも同じ
測定ディレクトリに保存する。大きいセーブ・資産・バイナリはbuild-trip-cache内だけに置く。
