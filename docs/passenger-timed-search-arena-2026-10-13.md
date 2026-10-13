# Timed Journey探索の連続配列とScratch再利用

ベースはA・B・D統合後の `jgrpp-ya`、`604ce993892479656a2c5ba2fbd311cb19c5d896`。
ブランチは `perf/passenger-timed-search-arena`。実マップで改善を確認したため採用する。

## データ構造と契約

StationIDは16bit。64駅ずつの配列ページを必要時に確保し、駅IDから直接参照する。
ページ内のhead/tailとLabelのnext indexで、旧駅別vectorと同じ挿入順に走査する。
検索後に初期化するのはTouched Stationだけで、全64,000駅の初期化は行わない。
高い駅IDが一つだけ現れても、そのIDまでの巨大な配列は確保しない。

Label、標準priority_queueのvector、Touched Station、次発索引のloading除外vectorをDBごとに再利用する。
simulation threadのみで使用する。別DBと比較用DBの領域は独立し、同じDBの再入時は一時Scratchへ退避する。
Scope終了時にLabelの経路情報を上書きして消し、全vectorの論理サイズとTouched Stationの行をクリアする。
結果のrouteは独立したコピーである。動的容量が8MiBを超えた検索は終了時に全領域を解放する。
Resetと正常Loadでも解放する。MSVC x64の固定管理領域はDBごとに8,312 bytes、駅ページは1,024 bytes。
8MiBの保持制限は動的容量に適用する。保存形式と保存バージョンは変更しない。

priority_queueは標準ライブラリのpush/pop/topを使い続ける。
比較キーは下界、費用、到着、乗車回数、最初の発車、ServiceID、乗車Order、降車Order、挿入indexの9項目を保持する。
Work、Label上限、駅別Label上限、探索メモリの判定位置と式も保持する。
各採用Labelの論理課金は従来の **1,536 bytes**。再利用済み容量を理由に割り引かない。
実運用のWork 65,536・Labels 8,192・8MiB探索予算を変更していない。

`PlanWithLegacySearchStructures()` は旧map・駅別vector・毎回確保するqueueを使う読み取り比較API。
新旧の探索本体は共通で、枝刈り・候補順・同点処理を同じ条件で比較する。
`passenger_journey_stats scratch_on/off` は保存しないローカル再利用設定。
無効時にも同じ配列構造を一時確保して同じ結果を返す。

## 正当性

700種類のWork上限、待機重み0/500、交互の2 CargoTypeについて、旧構造とcold/warm/Load済みpeer/全cache・再利用無効を比較した。
全route、ride、費用、到着、乗換回数、Work、Labels、Candidates、Memory、Deferred、理由、重複観測を照合する。
Label・駅別Label・Memory境界も比較した。既存の同点、繰返し停車、強制降車、同系統再乗車、Dispatch、貨物別制約にも旧構造比較を追加した。
同点を多く含む2,048要素×8回の交互push/popでは、標準queueとの全処理順を照合した。
高い駅ID、Touched Stationのリセット、過大容量の解放も確認した。

旅客回帰 **350 cases / 1,429,265 assertions**、最終製品構成のCTest **570/570成功**。
合成比較は1 case / 13,518 assertions成功。ゲームは計測オプションOFF、allocator計測はテストexeだけにリンクする。
初回の合成テストがcache無効設定を後続へ残した問題は、Scopeによる設定復元で修正した。
初回CTestの不完全な実行と、1,000停車の系統が公開Work上限で拒否された長路線標本は採用していない。
長路線は公開可能な250停車×32系統に直し、全標本でtimed service・既知位相・前向きLabelの存在を要求した。

## 同じ実マップ・1,024 tick×2組

MSVC 19.44 x64 Release、/MT、assert ON、LTO OFF、null video/sound/music。
旧→新→新→旧を逐次実行し、各run前とrun中にプロセス競合を確認した。競合検出0。
旧方式はベースコミットの探索翻訳単位を使用し、元のLabelサイズも保持する。
宣言の互換引数と同じ診断カウンターだけを加えた。詳細タイマーは速度比較に含めない。

StateTicks **3176545487→3176546511**。全4回とpolicy/次発索引/Scratch無効の末尾Saveは一致した。
SHA256は `971edd9083ab0a5084b99e0b7765497fe88013d47c62097c3005aebce4c54d5a`。
入力Saveは `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`。
固定資産257 filesの一覧SHA256は `de8e3fcab82414f994fd5a7e8e304f6e458bb70b3a8ceb8b6386b253305325f4`。
既存NewGRF容量警告と計測版のunpause処理を両方式で共通に使った。

| 指標 | 旧構造 | Arena |
|---|---:|---:|
| Label生成数 | 9,808,097 | 9,808,097 |
| Frontier検索数 | 162,380,466 | 162,380,466 |
| queue push / pop | 9,808,097 / 822,045 | 同じ |
| 探索Work | 486,821,299 | 486,821,299 |
| Found / Work / Label理由の結果数 | 9,352 / 5,525 / 35 | 同じ |
| 実乗車 / 乗換 / 配送people | 7,318 / 840 / 515 | 同じ |
| Timed前向き平均ms/tick | 12.189 | 7.932 |
| Journey平均ms/tick | 19.994 | 15.637 |
| simulation平均ms/tick | 43.311 | 39.263 |
| Journey p95 ms（2回） | 35 / 35 | 29 / 29 |
| Journey p99 ms（2回） | 46 / 45 | 39 / 38 |
| simulation p95 ms（2回） | 60 / 59 | 54 / 53 |
| simulation p99 ms（2回） | 70 / 69 | 68 / 63 |

前向き **34.92%**、Journey **21.79%**、simulation **9.35%**短縮。
p95/p99は1msヒストグラムの上端であり、正確な個々の標本値ではない。
一つのマップ・短い区間・2組の結果で、一般的な速度保証にはしない。

Scratch保持・高水位は **848,394 bytes（0.809MiB）**。無効runは保持0、検索中の高水位827,860 bytes。
プロセスのpeak working setは旧2.028GB前後、新2.030–2.035GBで、数MB増えた。
プロセス全体のメモリ低下は主張しない。100ms間隔のprivate memory標本と、動的容量の高水位は別の指標である。

256tickのキュー詳細ランは、両方式とも1,954,832操作。
計測経過時間は旧91.977ms、新72.682ms（0.359→0.284ms/tick）。
時計の観測コストを含み、exclusive CPU時間ではない。主な改善を独自ヒープの効果とは解釈しない。

## 8,000駅の合成測定

独立した4,000組、250停車×32路線、繰返し停車を含むハブの3負荷。各条件30組。
確保数はopt-in C++ allocator計測、時間はallocator観測を止めた別runから集計する。
比較APIの旧Labelにも8byteのnext領域があるので、この合成値を旧バイナリそのもののメモリ値とは呼ばない。
実マップの旧対照群は前述の元の翻訳単位を使う。

| 温間負荷 | 確保回数の中央値 旧→新 | 平均検索µs 旧→新 | 動的高水位 bytes 旧→新 | 保持 bytes |
|---|---:|---:|---:|---:|
| 独立駅組 | 12→3 | 3.26→1.76 | 1,000→1,840 | 1,600 |
| 長路線 | 535→3 | 66.12→32.36 | 161,309→106,092 | 105,852 |
| ハブ | 12,239→1 | 55,257.81→50,020.21 | 4,121,885→2,694,764 | 2,694,716 |

ハブの冷間も6.49%短縮、全cache・再利用無効は16.15%短縮した。
一方、独立駅組の全cache・再利用無効は **4.10→5.22µs（27.13%悪化）**。
少数駅では最初の1KiBページが旧mapより大きく、確保回数の減少だけでは改善を保証しない。
実運用の既定の再利用で改善し、保持量も上限以内なので採用する。無効時の小ODの退行は残る。
全条件のp95/p99、確保bytes、Labels、Frontier検索数は[集計](measurements/passenger-timed-search-arena-2026-10-13/synthetic-summary.json)を参照。

## Save/Load・初期参加・途中参加

同じ末尾Saveを私設127.0.0.1:17523サーバーでLoadし、2クライアントを接続した。
サーバーはcacheとScratch有効、両peerはpolicy/次発索引/OD fallback/Scratchを無効にした。
sync_freq=1、通常CPU割当て、試験用max_lag_time/max_join_time=32000。
サーバーは3176546511→3176547440の929tick進行し、終了時も両peerが接続中だった。
初期peerの最後の観測は3176547440、途中peerは3176547379で、128tick粒度の観測で61tick以内だった。
両peerでScratch保持0を確認した。同期不一致・assertion・切断の検出0。
peer側に15件のMissed frame for sync-testログはあるため、全tickのsync-testを観測したとは主張しない。
有限のloopback試験であり、WAN・長時間・1コアでの追従は未検証。

## 証跡と再現

[実マップ集計](measurements/passenger-timed-search-arena-2026-10-13/summary.json)、
[合成の全標本](measurements/passenger-timed-search-arena-2026-10-13/synthetic.log)、
[旅客回帰](measurements/passenger-timed-search-arena-2026-10-13/passenger-tests.log)、
[最終CTest](measurements/passenger-timed-search-arena-2026-10-13/ctest-product.log)、
[MP結果](measurements/passenger-timed-search-arena-2026-10-13/mp-result.json)。
Save・exe・NewGRFはコミットしない。

```powershell
cmake -S . -B build-timed-arena -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=PATH/TO/x64-windows-static" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DOPTION_NO_LTO=ON -DOPTION_USE_ASSERTS=ON -DOPTION_PASSENGER_BENCHMARK=OFF -DOPTION_SERVICE_INDEX_ALLOCATIONS=ON
cmake --build build-timed-arena --config Release --target openttd openttd_test regression_files --parallel 8
# 固定packageの準備は既存Run-Profile.py freeze / Prepare-Assets.pyを使用。
# regressionのroot buildとReleaseの双方にOpenGFXを置く。
ctest --test-dir build-timed-arena -C Release --output-on-failure -j 4
& ./build-timed-arena/Release/openttd_test.exe 'Timed search arena allocation and latency report' --reporter compact > build-timed-arena/synthetic.log
python docs/measurements/passenger-timed-search-arena-2026-10-13/Summarize-Synthetic.py build-timed-arena/synthetic.log build-timed-arena/synthetic-summary.json
& ./docs/measurements/passenger-timed-search-arena-2026-10-13/Build-Comparisons.ps1 -Source . -Build build-timed-arena -Dependencies PATH/TO/x64-windows-static
python docs/measurements/passenger-timed-search-arena-2026-10-13/Run-Comparison.py build-timed-arena --ticks 1024 --detail-ticks 256
python docs/measurements/passenger-timed-search-arena-2026-10-13/Summarize-Comparison.py build-timed-arena/comparison docs/measurements/passenger-timed-search-arena-2026-10-13
python docs/measurements/passenger-timed-search-arena-2026-10-13/Prepare-Multiplayer.py build-timed-arena
python build-timed-arena/Exercise-Pruning-Multiplayer.py build-timed-arena build-timed-arena/mp-arena --binary openttd-network.exe --backend periodic --input-save comparison/arena-1/arena-1-result.sav --duration 25
```

速度測定はビルド・回帰・MPと重ねない。最終ビルドはOPTION_PASSENGER_BENCHMARK=OFFへ戻して再検証した。
