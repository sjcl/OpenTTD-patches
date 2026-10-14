# 旅客経路の事前計算化：実装・検証と残る要件

対象は `jgrpp-ya` の `3f0c8aaac95058ec0b64040594c6a653275956e7` から作成した `feat/passenger-precomputed-routing`。未統合の timed-round-based ブランチは使用していない。

**共有経路表を実際の旅客の乗車処理まで接続した。全要件の完了・実マップ性能の合格はまだ宣言しない。** 特に途中折返しとの共通区間頻度の合算、完全な構築分割、駅内の変更Packetだけを処理するスケジューラ、実マップ輸送量比較、マルチプレイ途中参加の実機検証が残る。

## 切替

同期・保存対象の `linkgraph.passenger_journey_precomputed` を追加した。既定値は false。旅客Journey routingと自動旅客分配を有効にしたゲームで使用する。

```text
setting linkgraph.passenger_journey_routing true
setting linkgraph.passenger_journey_precomputed true
```

有効な表を持つODはTimed/Schedule全国探索や16 OD/tickの受付を通さず、共有表からServiceIntentを取得する。新方式を無効にすると派生表を解放し、既存方式を使用する。未完成・未対応の系統には既存の実車指令に基づくFallbackが残る。決定的なメモリ上限による表構築拒否では既存の受付付き探索へ退避する。

## 実装

### Phase 1：系統・Occurrenceグラフ

公開された `PassengerServiceDatabase` の一周分の停車イベントから構築する。同じ停車・時刻・CargoType別乗降条件・dispatch条件の系統をまとめる。複数の共有実車はグラフの状態数を増やさない。同じパターンの別OrderListにも乗車時に結び直せる。

駅の初期待機、直前の系統を記憶する乗換待機、各Order Occurrenceの到着・発車を別の状態にした。乗車時だけ待ち時間・混雑リスクを加え、車内の移動と停車は乗換にしない。強制降車は車内継続辺を遮断し、NoLoad/NoUnloadをCargoTypeのマスクで反映する。同じStationIDの再訪もOrder番号で区別する。Waypoint・Depot・運転停車の扱いは既存のperiodic compilerと実乗車検証を引き継ぐ。

待ち時間は既知の位相や認証済みdispatch slotの実間隔から算出する。重複位相を車両数で割って架空の頻度にしない。未知位相には保守的な周期コストを使い、Estimated intentの絶対発着時刻は設定しない。距離、乗車時間、待ち時間、乗換数は別々に復元できる。

**未達**：異なる途中折返しパターンとの共通区間の頻度合算は未実装。容量コスト・分散重みは最大個別容量を使う近似で、同時刻に来る車両群の総座席供給量を十分に集約していない。不完全なダイヤ・未対応指令を持つ系統は静的推定辺に補完せずFallbackへ回る。

### Phase 2：全目的地の逆向き経路表

StationIDを連続番号へ圧縮し、目的地ごとの逆向きDijkstraを行う。Labelは連続配列に保存し、ODごとのmap・経路コピーを確保しない。

直前に使った系統を待機状態へ含めたので、除外条件の救済は固定K候補への打切りに依存しない。等コストでは少ない乗車回数を選ぶ。近いコストの別初乗車パターンへの分散候補を追加し、Packet IDなどの決定的なハッシュで選ぶ。分散候補は経路の到達性を決めるK制限としては使用しない。

公開世代が変わると失効し、構築中は未完成として扱う。Label初期化、seed、heap操作、辺の緩和、代替候補の抽出を固定操作数で分割する。公開済み目的地行だけを参照できる。操作数・評価設定をJPSD v4に保存し、Load時は保存された公開DBから同じ操作列を再生して、同じ行とfrontierを復元する。構築未開始には明示的な保存マーカーを使う。Optional cacheの有無・実行時間は操作数や経路選択に使わない。

整数積・加算を確認した上で、全旅客CargoType合計の保守的な構築枠を2 GiBに制限する。判定は32/64bitで同じ固定バイト単価を使い、native sizeofは観測値だけに使う。実際のローカルOOMでは他クライアントと異なる経路を選ばず停止する。再開する場合はホスト側の同期設定で新方式を無効にする。JPSD v1〜3の読み込みは維持した。

大きな共有実車群を旧65,536構築Workで丸ごと省略しないため、新方式の世代構築に限って最大16,777,216 Workの原子的な系統取り込みを許可する。施設確認の見積もりも含め、毎tick同じ小さい枠で再試行し続けないようにした。公開実車の観測は、保存・構築時のID順序を確認した上で二分探索する。

**未達**：グラフ準備・系統取り込み・publication・Load時の再生は完全な分割処理になっていない。大規模ネットワークの受信継続と途中参加の時間上限は実機未検証。微小なダイヤ変更に対する追加の量子化・部分更新も未実装で、既存DBのepoch・同一publication判定に依存する。

### Phase 3：実際の経路・乗車

`PlanWaiting()` の通常経路を共有表へ接続した。表に有効な経路があればWork受付やOptional CacheのコールドWork再課金を受けない。乗換後も同じ表を参照する。最小乗換時刻、Service incarnation/generation、現在の指令・施設、容量・予約・実空席の検証は既存の実行経路に残す。Estimated intentを実車へ束縛して実際のReserve/Loadを行う。途中駅の継続・配送判定も既存のServiceIntent実行経路を使用する。

Timetable-awareな全国探索の削除やWork上限の引上げで通常取得を高速化した変更ではない。Timed/Schedule方式は無効時・例外時に残る。

### Phase 4：待機客と結合の索引

系統パターン別の待機Packet IDと、最小乗換時刻を待つキューを保持する。到着車両は自分の系統の候補だけを取り出す。同等パターンの別OrderListも共通候補を使用し、実乗車時に現在のサービスへ結び直す。Packet本体を複製して索引を作らない。

駅の主キューを安定したhandleを持つlistにし、索引候補を直接削除する。保存するPacket順・参照IDは維持する。従来ringとlistの参照リストのファイル型・エンコードは同じで、旧形式ロード時の変換も追加した。

Appendは厳密な結合属性のハッシュと残り人数枠で候補を絞り、最終確認は既存のTryMergeを使用する。分割、結合、人数の減少、予約の返却、乗車時の移動、削除、Save/Load、StateTicks補正に索引更新を接続した。支払・出発地・乗車履歴の結合条件を緩めていない。

**未達**：Fallback/Legacy集団は個々の系統別に分類されず、その集団の走査が残る。対象となる大量の候補を毎回vectorへ取り出すコストも残る。旧方式を選択しても主キューはlistなので、今回の索引対走査比較は元ブランチのring実装との比較ではない。

### Phase 5：イベント処理

新規待機客、世代・公開行更新、Intent期限、失効を契機に駅の経路パスを実行する。イベントがなければ常数時間の確認で戻る。通常の新方式ではInitializeFairSharesの全国censusと、空駅のwaiting-share処理を行わない。旧方式・メモリ拒否時の受付では従来Fair Shareを残す。新方式の有効表参照にはOD受付上限がないため、処理順の遅い駅をその予算で永久に拒否しない。

dirty、参照世代、公開行数、期限を保存する。Loadでは保存キューを再分類せず、索引だけを復元する。

**部分実装**：駅にイベントがあるパスでは既存の待機キュー全体を調べる。変更したPacketだけのキュー・期限別の失効処理・多数の更新に対する駅の分割スケジューラは残る課題。既存simulation自身の全駅処理は変更対象にしていない。

## 計算量とメモリ

Sを利用駅数、Mを停車Occurrence数、d(s)を駅の到着パターン数、b(s)を乗車Occurrence数とする。

- 状態数 V = S + Σd(s) + 2M。実車数は含めない。
- 辺数 E = O(M + Σ b(s)d(s))。大きなハブの乗換辺は二次に増える。
- 全目的地構築は O(S(V + E) log V)、表は概ね O(SV)。初期化と代替候補抽出も構築操作として数える。
- 最終目的地への通常取得は配列参照と定数個の選択・現在世代の検証。直前系統の指定時だけ待機状態の二分探索 O(log d(s))が加わる。
- 乗車候補取得は O(log パターン数 + 対象候補数 + 到来した待機イベント数)。Fallback集団は別途走査する。
- 結合はハッシュ参照と同一属性内の残り人数枠検索。ハッシュ衝突には厳密な条件検査があり、最悪時の候補走査は残る。

`flat_array_bytes` は主要配列の容量計測で、すべてのmap/list/queueの実メモリではない。`allocation_envelope` は構築・表の保守的な事前枠。OSのプロセスpeakは別に測定する。

## 検証

MSVC x64 Release、assert ON、LTO OFF、製品のPassenger Benchmark OFF。VSの実行ファイル出力先をデータのあるビルドルートへ揃えた。Releaseサブディレクトリの初回起動テスト3件のtimeoutを成功として扱わず、配置修正後に全体を再検証した。型変更を混在させた途中のビルドで発生した回帰クラッシュも最終の成功結果と分けて記録した。

最終製品ソースで全CTest **617/617** が成功。旅客フィルタは **393ケース・2,741,014アサーション**、新方式は **11ケース・13,161アサーション**。構築の1操作分割と一括処理、途中構築の保存再開、旧保存形式、乗降マスク・強制降車、Occurrence、再乗車除外、実Reserve/Load、部分予約・返却、同等系統への実車束縛、待機時刻、イベントなしの100tick、600台の共有系統を確認した。

マルチプレイの決定性は派生表の再生・cache非依存の回帰まで。新方式でのserver/client途中参加はまだ実施していない。

## CPU合成計測

生ログと機械可読結果は [cpu/result.json](measurements/passenger-precomputed-routing-2026-10-15/cpu/result.json) と [cpu/raw.log](measurements/passenger-precomputed-routing-2026-10-15/cpu/raw.log)。lookup値は通常の診断計測を含むelapsed ns。Process CPUはWindowsのGetProcessTimesで独立に測定する。CPU時間・Work・simulation時間を混同しない。

| 利用駅 | 初期構築 ms | 取得 ns | p95 ns | p99 ns | 主要配列 MiB | 構築枠 MiB |
|---:|---:|---:|---:|---:|---:|---:|
| 14 | 0.144 | 61 | 66 | 71 | 0.53 | 1.08 |
| 62 | 0.293 | 62 | 68 | 124 | 1.11 | 2.09 |
| 254 | 3.271 | 64 | 123 | 128 | 10.64 | 17.57 |
| 510 | 13.596 | 61 | 70 | 91 | 41.27 | 66.65 |

| 対象外待機Packet | 従来走査 µs | 索引参照 µs | 1回の候補検査数 |
|---:|---:|---:|---:|
| 128 | 10.912 | 0.231 | 1 |
| 1,024 | 91.621 | 0.235 | 1 |
| 8,192 | 718.384 | 0.233 | 1 |
| 32,768 | 3945.404 | 0.243 | 1 |

同一系統の合成実車256〜8,192台で取得は61〜66 ns。状態数8・辺数8・主要配列約0.50 MiBは一定。全計測プロセスのCPUは2.938秒、elapsedは3.259秒、OS peak working setは58.28 MiB。OS peakはテストランタイム・先行ケースを含む。


駅数の軸は利用駅14〜510を増やすため、無関係な2駅系統も増やす。駅数だけを変えて全系統数を固定する試験とは異なる。実車数の軸は同一位相・一意IDの合成車両256〜8,192で、DB取り込みとは別に表構造を試す。実DBの取り込みは600台の共有系統回帰で確認する。同一ODの取得件数とOD種類の軸も実行した。

これらの結果は経路表・索引の計算量を確認する合成試験で、元ブランチとの実マップsimulation比較ではない。対象外Packetを増やす乗車試験は対象候補1件を固定し、warmup1回と512回の計測で毎回1候補を検査する。

**未測定**：実マップの輸送量・乗換・配送人数、simulation平均/p95/p99、長時間運転と連続路線変更、極端な迂回・長時間待機、構築フェーズ個別のOS peak。過去レポートの固定セーブのフォルダがこの環境になく、比較用セーブのパスを問い合わせている。輸送人数が減ったことによるCPU低下を成功とは判定しない。

## 再現

```text
cmake -S . -B build-precomputed -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR=<installed> -DOPTION_USE_ASSERTS=ON -DOPTION_NO_LTO=ON -DOPTION_PASSENGER_BENCHMARK=OFF -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=<absolute-build-directory>
cmake --build build-precomputed --config Release --target openttd openttd_test regression_files --parallel 8 -- /nodeReuse:false
ctest --test-dir build-precomputed -C Release --output-on-failure -j 8
build-precomputed/openttd_test.exe "[passenger]~[.benchmark]"
python utils/passenger_precomputed_benchmark.py build-precomputed/openttd_test.exe <output-directory>
```

Phase 1〜5は独立コミットに分割した。追加の世代構築・観測検索修正と計測結果も別コミットに記録する。既存Timed/Schedule/Fallback・CargoPacketの支払処理・ネットワークtimeoutは削除・延長していない。
