# 旅客事前計算表の初期構築を目的地単位で並列化

基点は `sjcl/jgrpp-ya` の `093218f882`。
ブランチは `perf/passenger-precomputed-parallel-build`。

## 実装範囲

`PreparePrecomputed()` の初期構築バリアで、LegacyのDijkstraとHyperpathのイベント探索を複数CPUスレッドへ分配する。
グラフ準備、Legacyの代替候補抽出、Policy圧縮、Rowコピー・公開、逆引き到達先・乗車マスク更新、旧Row退役はメインスレッドで行う。
通常tickの更新と `StepLoad()` は従来の逐次経路を維持した。
初期探索の正しさと速度を優先し、ロード再生と通常tickへの非同期先行計算は今回の対象に含めない。

`DestinationWorkspace` がSearch SoA、Strategy、queue、候補配列、作業Row、カーソル、探索カウンターを所有する。
探索関数は `const PassengerPrecomputedTable &` に相当するconstメソッドと明示的なWorkspace引数に分け、逐次版とWorker版で共用する。
Workerは確定済みの駅・pattern・occurrence・revisit・CSR・seed等を参照するだけで、グラフのコピーやライブ駅・車両・サービス参照を持たない。

設定はローカルの `gui.passenger_precomputed_workers`（0〜8、既定4）。0は既存の逐次 `Step()`。
`NotInSave` と `NoNetworkSync` を指定し、JPSDやゲーム内設定の同期に含めない。
要求数に加え、CPU数、残り目的地数、グラフサイズから推定する512 MiBの追加scratch目安で同時実行数を制限する。
この目安は物理メモリ使用量の保証ではなく、経路の論理的なメモリ入場判定にも使用しない。

## Workと公開

1バッチの未公開探索結果は最大Worker数の2倍。各結果は探索を終えたWorkspaceと正確な探索Workを持つ。
メインスレッドは目的地順に従来の `BUILD_WORK = 65536` までを消費し、探索Workの境界で初めて結果を採用する。
その後のコピー・候補処理・公開には既存のstate machineを使うため、Row公開だけでなく途中の到達先配列・乗車マスク更新も同じWork位置になる。
速いWorkerの結果を先に公開することはない。

探索途中の仮想Workだけを消費した時点では、メインのWorkspaceはそのRowの開始状態のまま残す。
通常の `Step()` に戻す場合は、このWorkspaceで消費済みprefixを再探索してから続行する。この再探索を `operations` に加算しない。
セーブには従来と同じ総Workを保存し、ロードは従来の逐次探索で同じprefixを再現する。未公開結果やスレッド状態の保存は追加しない。

各呼び出し中のWorkerはすべてjoinしてから戻る。したがって、Reset、Discard、テーブル移動後に動作中のWorkerやグラフ参照が残らない。
Workspaceは次のバッチで再利用する。スレッド自体はバッチごとに起動する。
起動失敗、追加メモリ確保失敗、Worker例外ではjoin後に結果を破棄し、その世代では逐次探索へ戻す。
例外時は他の探索も65536 Work以下の探索スライスの間で停止する。
逐次経路自体でも物理メモリが足りない場合は従来どおり停止し、クライアントごとに異なる経路を選ぶフォールバックにはしない。

## 検証

通常のネイティブテストに、次を追加した。

- 全RowのLabel全フィールド、alternatives、例外Label、Policy・候補配列、到達先逆引き、乗車マスク、公開ビットの完全比較。
- 0・1・2・4・8 Worker、Legacy・Hyperpath、flat・完全Policyで同一のスライス境界を比較。
- 基点のソースから生成した独立TUとの比較。変更後の逐次版同士だけを比較する検査ではない。
- 仮想Work途中の逐次復帰、途中prefixの再生、Reset、遅いWorkerと逆転した完了順、Worker例外、全起動失敗・一部起動失敗、メモリ拒否。
- 異なるローカルWorker設定での初期DB保存、参加peerの逐次再生、通常更新時の保存結果一致。

独立参照TUは `utils/passenger_parallel_reference_prepare.py` で生成し、そのCMake includeを指定したテストにだけリンクする。
通常の製品には含めない。JPSD形式、`passenger_periodic` feature、ロードWork上限、ネットワークの参加絶対期限は変更していない。

最終ビルドの非hiddenテストは735件、10,115,392アサーション成功。
通常tickの旧世代退役、途中保存、旧形式の再生・不正Work上限の既存テストも含む。

## 実機マルチプレイ

vanillaの `regression/stationlist/test.sav` を、コピーした計測用TUのfixtureコマンドで3駅の固定時刻入力にした。
サーバーのローカル設定は8 Worker、クライアントは1 Worker。3目的地なのでサーバーの実行数は目的地数でも制限される。
既定の接続タイムアウト、`frame_freq=1`、assert ONでloopback接続を行った。
途中参加、接続後の無効化→再有効化バリア、transfer penalty変更後の通常tick更新を比較した。

| モード | 参加時StateTicks | 終了時StateTicks | 進行tick | 公開Row | 総Work |
|---|---:|---:|---:|---:|---:|
| Legacy | 13,639,680 | 13,640,764 | 1,084 | 3 | 197 |
| Hyperpath | 13,639,680 | 13,640,766 | 1,086 | 3 | 266 |

両方式で、参加直後・再有効化直後・輸送進行後の比較に不一致は0件。
StateTicks、DB保存digest、Row数・Work、packet内容、所有先、積み降ろし、配送数、運賃情報が一致した。
network errorとdesyncは0件。バリア中のStateTicks先行もなかった。
このfixtureの旅客ODは未設定のため、実機試験は主に経路表・保存状態の同期確認であり、実マップでの旅客経路選択・輸送負荷の網羅試験ではない。
旅客の候補・Label・乗車結果はネイティブの全フィールド比較と既存輸送テストで検証した。
生記録の `mp-legacy`、`mp-hyperpath` には結果JSON、両peerのログ、保存状態の読み取り結果を保存した。素材・セーブ・dumpは含めない。

既存の古いfixtureを使った試行は、素材junction切れと別系統の `passenger_od_assignment` feature v3によりロード前に失敗した。
また、既存の計測用 `fixture_timetable` がtotal-duration cacheを更新していなかった問題をassertで検出し、計測フックを修正してから上記の試験をやり直した。
製品のassertや保存feature検査は弱めていない。これらの失敗試行は性能・同期の成功数に含めない。

## 合成ベンチマーク

既存の `SyntheticNetwork` に32×32、240系統、固定seedを渡した894駅の同一入力を使う。
MSVC x64 Release、assert ON。各モード・Worker数を新規プロセスで3回ずつ実行し、実行順を巡回させた。
wall時間はグラフ準備開始から全Row公開完了まで。
process CPUとpeak working setはWindows APIで別に計測し、テストランタイムと最終digest走査も含む。
生記録は `measurements/passenger-parallel-build-2026-10-18`。

CPUはAMD Ryzen 9 9950X、16 core / 32 logical CPU。時間は3回の中央値、メモリはMiB。
他プロセスの活動やクロック変動を排除していないため、単発の小差は改善の根拠にしない。

| 方式 | Worker設定 | 構築wall秒 | 逐次比 | process CPU秒 | peak working set MiB |
|---|---:|---:|---:|---:|---:|
| Legacy | 0（逐次） | 0.8902 | 1.00x | 0.9219 | 120.05 |
| Legacy | 1 | 1.0296 | 0.86x | 1.0469 | 122.29 |
| Legacy | 2 | 0.8150 | 1.09x | 1.2813 | 124.80 |
| Legacy | 4 | 0.6146 | 1.45x | 1.3750 | 128.76 |
| Legacy | 8 | 0.5382 | 1.65x | 1.2813 | 136.29 |
| Hyperpath | 0（逐次） | 0.9373 | 1.00x | 0.9844 | 213.31 |
| Hyperpath | 1 | 1.2789 | 0.73x | 1.3281 | 217.80 |
| Hyperpath | 2 | 0.9558 | 0.98x | 1.7344 | 220.18 |
| Hyperpath | 4 | 0.5379 | 1.74x | 1.4531 | 225.15 |
| Hyperpath | 8 | 0.4562 | 2.05x | 1.4531 | 234.52 |

Legacyの4/8 Workerはwallがそれぞれ31.0% / 39.5%減った。Hyperpathは42.6% / 51.3%減った。
8 WorkerのCPU時間は逐次比でLegacy約1.39倍、Hyperpath約1.48倍。
peak working setの増分はそれぞれ約16.24 MiB / 21.21 MiBで、グラフをWorker数分複製していない。
全30試行で各モード内の全保存Row・マスク・逆引きのdigestと総Workは同一だった。
総WorkはLegacy 71,559,024、Hyperpath 67,268,128。

| 内訳（中央値） | Legacy 4 | Legacy 8 | Hyperpath 4 | Hyperpath 8 |
|---|---:|---:|---:|---:|
| グラフ準備 ms | 213.40 | 212.19 | 2.27 | 2.29 |
| バッチ探索・起動・join wall ms | 281.17 | 201.76 | 332.43 | 226.64 |
| Worker探索elapsed合計 ms | 875.27 | 1,178.42 | 1,104.97 | 1,351.32 |
| 公開・統合等のメイン処理 ms | 119.48 | 130.39 | 201.53 | 222.82 |

Worker探索elapsed合計にはWorkspace開始処理も含む。バッチwallから探索elapsedの合計を割り引いた残りには、起動・join・負荷偏り・待機が含まれ、OSのスケジューラ待ちだけを分離した値ではない。
表の中央値から `探索elapsed合計 / (設定Worker数 × バッチwall)` を求めると、Legacyの4/8 Workerは約77.8% / 73.0%、Hyperpathは約83.1% / 74.5%の稼働目安になる。
同じ中央値から求めた残りのWorker時間合計は、Legacy約249 / 436 ms、Hyperpath約225 / 462 ms。これは待機に加えて起動・join等を含む概算であり、末尾バッチのWorker数減少も厳密には補正していない。
逐次設定0では探索と公開の個別タイマーを追加しておらず、両者の合計は `row_ns` で記録する。
目標の2〜4倍に届いたのはこの合成入力の8 Worker Hyperpathで約2倍までであり、全モード・全入力での達成は主張しない。

目的地ごとの探索WorkはLegacy 69,837〜70,120、合計62,573,430。Hyperpath 62,757〜63,020、合計56,231,397。
探索終了後の候補処理・コピー・公開Workはそれぞれ8,985,594 / 11,036,731。
この入力のWork偏りは小さく、実マップの不均一な探索負荷を代表するとは限らない。
互換性のある固定実マップfixtureが手元に残っていなかったため、実マップの長時間性能比較は実施していない。

再現例：

```text
python utils/passenger_parallel_reference_prepare.py BUILD
cmake -S . -B BUILD -DCMAKE_PROJECT_INCLUDE=ABSOLUTE_BUILD/parallel-reference/reference.cmake
cmake --build BUILD --target openttd_test
openttd_test "~[.]" -r compact
python utils/passenger_parallel_benchmark.py BUILD/openttd_test.exe OUTPUT --repeat 3
```

比較に使ったソースのSHA-256とテストログも生記録に含める。

## 制約と次の最適化

グラフ準備と公開処理は逐次なので、それらの比率が速度向上を制限する。
特にLegacyでは共通区間の準備が残る。スレッド起動、join、バッチ末尾の待機にも費用がかかる。
1 Workerの専用スレッドは逐次版と同じ探索量にこれらの費用を追加するため、速くなるとは限らない。
メモリ帯域、allocator、ホストの他の負荷も並列時の合計CPU時間を増やす。

次の候補は、バリア内で寿命を閉じる再利用スレッドプール、Row完成処理の分離、通常tickの有界な非同期先行計算である。
いずれも現在のWork境界・prefix再生・旧Row退役を保つ検査を先に通す必要がある。
今回の結果から通常tickやロードそのものの速度改善は主張しない。

## Commit

- `6ccc112aba` — 探索Workspace分離。
- `8868d7c1c9` — 有界な目的地WorkerとWorkの仮想消費。
- `bcfd1bb2f8` — 初期構築バリアとローカルWorker設定。
- `52f201b386` — 保存・公開・独立参照・異常系の比較。
- `3281f8c7ed` — 起動失敗、キャンセル、scratch上限の検証・修正。
- `cae824221b` — 異なるWorker設定のloopback試験。
- 本レポートと合成ベンチマーク・生記録のcommit。

push先は `sjcl/OpenTTD-patches` の `perf/passenger-precomputed-parallel-build`。
この作業ツリーの `origin` は別forkを指していたため、指定リポジトリを指す `sjcl` リモートを使用する。
