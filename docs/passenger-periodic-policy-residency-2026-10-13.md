# 周期JourneyのODポリシーを長期間再利用する

`jgrpp-ya` の `208f7378a3` をベースに、`perf/passenger-periodic-policy-residency` のmanaged worktreeで実装した。Timed候補探索修正 `278044f85f`、Schedule-aware、実停車制約を含む。

同一実マップ・8192 StateTicksで、Journey平均 **20.839 → 12.861 ms/tick（38.3%減）**、simulation平均 **43.292 → 33.772 ms（22.0%減）**。Timed検索だけでは **9.429 → 8.406 ms/tick（10.9%減）**。Schedule検索は **7.507 → 0.947 ms/tick（87.4%減）**。**Journey 5 ms/tick未満という目標は未達**で、残る最大の負荷は時刻依存のTimed前向き探索である。

キャッシュ有効・無効と保持配列圧縮後の3ランは、最終セーブのSHA-256まで一致した。キャッシュの存在による旅客削減、低Work扱い、候補の間引きは導入していない。

## 元の全破棄

`Invalidate()` が構造変更ごとに両方式をクリアし、`Tick()` が時刻更新の置換開始と各Service公開時にもクリアしていた。位相ずれの観測も構造変更扱いだった。TimedのLRUは `min_element` による全件走査、Scheduleは先頭キーの削除だった。

速度比較と別のベース診断コピーで、1530 ticksとロード時のクリア呼出しを記録した。実行コードへのログ追加があるため、このランの時間を性能比較に使わない。

| 理由 | 呼出し | 空でない破棄 | Timed喪失 | Schedule喪失 | 1回の最大喪失 |
|---|---:|---:|---:|---:|---:|
| 構造変更通知 | 1769 | 640 | 21041 | 12516 | 406 |
| Service公開 | 5612 | 145 | 2316 | 1545 | 32 |
| 時刻置換開始 | 547 | 3 | 258 | 165 | 372 |
| 全体変更 | 6 | 2 | 170 | 81 | 128 |
| Reset | 6 | 0 | 0 | 0 | 0 |
| 合計 | 7940 | 790 | 23785 | 14307 | 406 |

[理由別JSON](measurements/passenger-periodic-policy-residency-2026-10-13/baseline-clear-summary.json)と再現用 `Instrument-Baseline.py` を保存した。元ソースは変更せず、コピーした翻訳単位と生成プロジェクトを使う。速度比較用ベースexeは診断を加える前の複製。

## 構築と公開

- `services` / `stations` は公開済みDB、`building` は構築済み未公開レコード。Service構築の `generation` と公開ネットワークの `published_revision` を分けた。
- 4096 StateTicksの固定epochで更新キューを凍結する。追加通知は次のキューへ入り、連続する自動時刻更新が現在のバッチを飢餓させない。構築は従来と同じ65536 Work/tick。構築完了後（削除レコードでステージング枠に達した場合は先にその枠を排出）、公開も旧・新停車索引、fleet、dispatchブロックに固定Workを課金し、予算内のService群をまとめて切り替える。時刻やCPU速度を公開判定に使わない。
- 初期発見は予算内で逐次公開を維持し、既存の初期輸送を止めない。施設等の全体変更も通知されたStateTicksから固定Workで再検証する。
- 構造変更されたServiceは `unsafe` に入り、公開待ちの古いDBからの実乗車を直ちに拒否する。OrderListの有効ID・incarnation、駅・車両の生存、実指令の乗降可否、容量・施設・prefixを引き続き検証する。計画はコピー済みIDと時刻を読み、削除済みオブジェクトへのポインターを保持しない。
- 位相ずれは時刻変更へまとめる。整数周回だけ異なる捕捉と再検証後に同一のレコードは旧Service世代を維持する。
- JPSD形式3へ更新。公開DB、構築済みレコード、未処理キュー、待機開始ticks、cursor、公開revision、bootstrap/publishing、unsafeを保存する。形式1/2も読める。公開DBと構築DBには各64 MiBの枠があり、wire上限を128 MiBとした。これはポリシー枠とは別。

## 局所失効と決定性

Timed・Scheduleの両方で、コールド計算が参照した駅のRevisionを記録する。公開変更で旧・新Serviceの交換駅をstampし、参照駅のstampが変わったポリシーだけ次の照会で捨てる。新規Serviceも新しい駅索引行をstampする。

選ばれた経路への依存だけを見る方式ではない。逆DijkstraやSchedule探索で一度読んだ駅行が変われば、短絡路線、別経路、Work数の変化も再計算対象となる。読んだ行がすべて不変なら、決定的な探索の各ステップ、停止点、結果、論理Workが同じになる。このため途中で打ち切られた探索や失敗ポリシーも再利用できる。無関係な成分の更新では両方式がヒットし、新短絡路線で既存の未使用Serviceへの経路が改善する回帰を追加した。

キャッシュは未保存の派生アクセラレータのまま。**ヒットでもコールド計算のWorkと論理探索メモリを再現する**。Timedは毎回ready時刻に応じて前向き探索する。Scheduleは保存済みの計算Workを払い、not_before/planned_atを現在のreadyへ更新する。既存の候補、label、transfer、memory、tick quotaは維持する。

両方式を1つのLRUへ統合し、合計4096 entries / 16 MiB以内とした。entry storageは15 MiB、駅stampと最大4096 ODの再構築観測に1 MiBを予約する。下界と依存stampはソート済み配列で保持する。コールド逆探索は従来の木と同じ順序・Work・128 bytes/settled stationの論理メモリを払い、保持だけを16 bytes/下界pairへ圧縮する。全件LRU走査を除去し、退避時の大量ノード解放も削減した。

`passenger_journey_stats cache_on|cache_off` はローカルの派生キャッシュだけを切り替える。その設定はLoadをまたいで保持するが、セーブには入らない。再構築回数は固定サイズの観測履歴内の重複であり、過去全ODに対する無制限の厳密総数ではない。失効のための全国走査や毎tickのOD全件再評価は加えていない。

## 実マップ

同じ `verified-result.sav` のコピー（SHA-256 `14D68C82911B9DCFC5EF7291E9697C5BBD67F44FFCC79D7575A144405EDF6390`）、StateTicks 3176545487 → 3176553679、8192 ticks。旅客CargoDist automatic、OD/Journey/periodic有効、recalc_interval=8、recalc_time=32、MSVC 19.44 x64 Release /MT、assert ON、LTO OFF、null video/sound/music。各ランを逐次実行。セーブ・NewGRF・exeはコミットしない。

元マップには既存のNewGRF欠落・容量変更警告があり、両側で同じ資産と入力を使用した。元セーブのhashは実行後も不変。これは完全なGRF一致環境での全マップ保証ではなく、この同一環境での比較である。以前の1527 ticks・Journey 13.6 msという値に、今回の長い併用区間の数値を直接比率換算しない。

| 8192 ticks | ベース | 初回保持版 | 最終圧縮保持版 |
|---|---:|---:|---:|
| simulation平均 / 最大 ms | 43.292 / 98.431 | 37.806 / 109.467 | 33.772 / 87.944 |
| Journey平均 / 最大 ms | 20.839 / 60.969 | 16.611 / 86.466 | 12.861 / 63.356 |
| Journey p95 / p99 ms | 39 / 45 | 37 / 52 | 26 / 38 |
| Timed検索 ms/tick | 9.429 | 11.031 | 8.406 |
| Schedule検索 ms/tick | 7.507 | 1.967 | 0.947 |
| Timedヒット率 | 5.97% | 45.96% | 61.92% |
| Scheduleヒット率 | 7.84% | 61.80% | 79.49% |
| 通常tickのServiceGraph構築 | 0 | 0 | 0 |
| 終端の共有cache entries | 別枠 | 453 | 2039 |
| cache footprint bytes | Timed 673920 + Schedule 13312 | 16388584 | 16641752 |
| LRU退避 | 未計測 | 102401 | 56680 |
| LRU処理 us（全区間） | 未計測 | 2258122 | 94996 |
| 新版の全クリア / 喪失 | — | 0 / 0 | 0 / 0 |
| 全体再検証通知 | 27 | 27 | 27 |
| 構築 / Service公開変更 | 26256 / 未計測 | 19555 / 9525 | 19555 / 9525 |
| 実時間 s / CPU時間 s（ロード含む） | 372.793 / 480.672 | 326.756 / 433.328 | 293.680 / 399.453 |
| process peak Working Set bytes | 2068860928 | 2082148352 | 2087849984 |

圧縮版はLRU処理時間を初回保持版から95.8%減らした。LRU退避自体はまだ多く、16 MiBを使い切る区間がある。上限を引き上げて改善を装ってはいない。最終版の追加837 / 削除0 / 変更9525、時刻通知52686、終端更新待ち416 tasks、最大観測待機1266 ticks。公開待機合計4601484 ticks / 19299 samples（平均約238.4 ticks）。通知には位相観測と自動時刻更新が含まれ、実際に値が変わった時刻表数と同一ではない。

## 輸送とキャッシュ有無

| 人数 | ベース | 修正版（有効・無効・圧縮後の全てで一致） |
|---|---:|---:|
| Timed適用 | 100333 | 101513 |
| Timed実乗車 | 34696 | 33511 |
| Timed乗換 | 15812 | 14456 |
| Timed最終配送 | 11395 | 10001 |
| Schedule適用 | 135 | 66 |
| Schedule実乗車 / 乗換 / 配送 | 35 / 6 / 0 | 8 / 6 / 0 |

ベースとの人数差には公開・位相更新時期の変更が含まれ、旅客状態は途中から分岐する。この区間ではTimed配送が減っていることを隠さない。更新遅延を許容する設計の比較であり、ベースからの輸送人数維持・増加を達成したとは主張しない。

純粋なキャッシュ有無では、131072 Timed検索、94417 Schedule検索、DB構築Work 73098752、Schedule Work 1367508622、全適用・実乗車・乗換・配送人数が一致した。**初回保持有効 / 保持無効 / 圧縮保持有効の終端セーブは全て `665316AA3DE1B998A8B0EF13548C74F4948ADCE8BC0A599FE617BE777DA3D078`**。キャッシュを有効にしただけで少ない旅客や違うゲーム状態にする処理はない。

## 残る段階別負荷

| 最終版の計測scope | ms/tick |
|---|---:|
| Timed逆ポリシーmiss構築 | 2.235 |
| Timed前向き探索とその他（逆構築を除く） | 6.171 |
| Schedule全検索（うち構築0.931） | 0.947 |
| DB構築・公開 | 0.035 |
| Journeyその他（上記をJourney平均から引いた概算） | 3.473 |
| Journey平均 | 12.861 |

共有LRUは0.0116 ms/tick、stamp検証は0.0053 ms/tickで、上の照会scope内と重複するため加算しない。waiting / boarding / fallback_distanceも包含scopeであり、単純加算しない。

Timedのnext_departuresは179819090回（約21950/tick）。時刻別の候補・ラベル探索、物理位相評価が依然として毎照会で必要。次段階はこの前向き探索・位相評価の物理CPU削減と、16 MiB内のOD保持効率である。保存されないキャッシュの論理Workを減らす回避策は採らない。残る約3.47 msには実乗車/prefix検証、OD fallback等が含まれる。今回のキャッシュ変更だけで5 ms/tickを達成したとは報告しない。

## 検証

- `[passenger]`: **320 cases / 977501 assertions**。
- 最終製品版（計測オプションOFF）CTest: **533/533**、81.76秒。旧/current save、GUI/診断不変性、無効時CargoDist、実積載、乗換・配送、script互換を含む。
- 無更新12000 ticks後の両方式ヒット、無関係成分の更新、未使用の新短絡路線、追加更新中の凍結バッチ、構築途中のSave/Loadとcold peer進捗一致、負ticksとrebasing、削除された駅・車両・OrderList、NoLoad、連続自動測定、64 MiBステージング満杯時の削除・途中保存とpeer進捗一致、5000異なるODの混合LRU、キャッシュ無効時のWork/経路/延期/メモリ一致を確認。
- 実マップとGRFなし制御マップで、**初期参加と途中参加の2 clients、各1 core、ホストcache有効・clients cache無効、sync_freq=1、検出された同期失敗なし**。localhost 127.0.0.1のみ。Load前のon_clientスクリプトでcache_offを適用し、設定がLoad後も残ることは回帰でも確認した。ネットワークexeはstdio取得用にPE subsystemだけconsoleへ変更したコピーで、コピー元との実行コードは同一。計測後にローカルcache設定のLoad保持、network試験後に64 MiBステージング満杯時の削除境界を追加した。境界修正は通常の8 MiB前後の実マップ構築領域では発火せず、別の保存境界回帰で検証した。

[集計と生ログ](measurements/passenger-periodic-policy-residency-2026-10-13/summary.json)、[ハッシュ・条件](measurements/passenger-periodic-policy-residency-2026-10-13/verification-metadata.json)、[製品CTest](measurements/passenger-periodic-policy-residency-2026-10-13/ctest-product.log)を保存した。clientの遅延・取り逃したsync frame数も集計JSONへ含める。短いloopback試験を長期マルチプレイ耐久試験と同一視しない。

実行例（同じ依存ライブラリを指定してconfigure済み）:

```powershell
cmake --build build-policy-residency --config Release --target openttd openttd_test regression_files --parallel 8
build-policy-residency/Release/openttd_test.exe '[passenger]' --reporter compact
ctest --test-dir build-policy-residency -C Release --output-on-failure -j 8
```

計測には `OPTION_PASSENGER_BENCHMARK=ON` の複製を使い、`Measure-Residency.ps1 -Ticks 8192` で入力コピーを進める。`-DisablePolicies` は派生cacheだけを無効化する。速度計測は逐次実行し、baseline traceとnetwork試験は別に行う。製品のオプション既定値はOFFのまま。
