# Timed Journeyの発車計算前の安全な枝刈り

ベースはAgent Aの次発索引とAgent BのFallback変更を含む最新 `jgrpp-ya`
`56b0f287b013d4775cbd62c4ec5d5801c03f8e40`。作業ブランチは
`perf/passenger-timed-pruning`。全国ServiceGraph・将来Visit列は追加しない。

## 下界と上界

前向き探索でMemory/Label admissionを通過した目的地ラベルの最小費用を上界にする。
まだキューから取り出していなくても、保持済みの完全経路は実行可能な上界である。
ラベルの採用・最終タイブレークは従来のpop時の比較に任せる。

各乗車Occurrenceについて、同じ一周内の降車可能Occurrenceを順に評価する。
乗車から降車までの設定所要時間は到着offset－乗車departure offsetに、必要なら一周期を加える。
途中のdwell・通常車庫・Waypointもoffsetに含まれる。繰返し駅を一つにまとめない。
NoUnloadの駅は出口にせず、貨物別Forced Transfer/Unloadで走査を止める。
乗車側のNoLoad・初乗車のexcluded service・最大乗換回数は既存の判定を先に適用する。

降車先の安全な逆下界を加え、既に発生した費用も加える。
乗換では、最低乗換時間、乗換ペナルティ、最低乗換時間の追加重み、
最低乗換時間に対する待機重みの整数切捨て分を加えられる。
目的地以外への降車後にも、続行には少なくとももう一乗車が必要なので同じ最低乗換費用を加える。
実際の費用式は各乗換でこれら以上の非負費用を課す。同系統への再乗車も例外にならない。

その他の待機は0へ緩和する。これは下界であり、発車時刻や確定待機時間ではない。
全ての出口の下界が上界を**厳密に超える**とき、位相照会の前に系統を除外する。
同費用を残すので、到着時刻・乗換回数・最初の発車時刻・ServiceID・乗降Order Indexの比較は失われない。
最初の有望な出口で評価を終了する。また一周の到着offsetは単調なので、走行時間だけで上界を超えたら後続の走査を終了できる。
非負費用の加算overflowは表現可能な上界を超える証拠になる。

逆探索で保持するのはsettled距離だけである。Work/Memory中断時に未確定駅を到達不能にせず、距離0を使う。
正常にoriginをsettleして停止した場合だけ、未確定駅に最後のsettled距離 `unsettled_bound` を使う。
非負辺のDijkstraでは、その時点の未確定距離はこの値以上である。
本来の途中中断にもこの単調性は使えるが、今回の実装は途中中断では保守的に0へ落とす。

## Workと決定性

これはCPUだけの最適化ではなく、**探索Workを減らすアルゴリズム変更**である。
系統のvisitに従来どおり1 Workを課し、枝刈りした車両・Dispatch slot・降車先のvisitは課金しない。
下界の評価は既存 `bound()` と同様にWorkへ追加しない。評価は公開済みの一周だけを参照し、
compile/loadで制限する4096 stops以内、追加動的領域なしで終了する。
Work不足時の採用、Label/Memory制限への到達、未知位相の観測件数は旧方式から変わり得る。

判断は公開済み時刻・貨物別マスク・確定ラベル・ポリシーの数学的下界だけから行う。
時計、物理計算カウンター、車両位相索引やポリシーのキャッシュ有無は判断に使わない。
枝刈りされない候補はAgent Aの索引と、その旧走査Fallbackを利用する。
JPSD保存形式、公開世代、スケジューラー、RNG、Schedule-awareのアルゴリズムは変えない。
Timedが成立すれば優先し、Timedが延期・不成立なら既存のSchedule-aware/Fallbackへ進む。

`PlanWithoutServicePruning()` は明示的な読み取り比較用で、simulationやconsoleの設定には接続していない。
この比較は保守的な部分逆下界の修正を共有する。実マップの旧バイナリにはベースコミットの
Plan翻訳単位そのものを使用し、部分逆下界修正も含めた全変更との差を測る。

診断 `Periodic service pruning` の `pruned_services` は乗車Occurrenceごとの系統除外数、
`skipped_vehicle_visits` は未走査の公開fleet候補の総数である。旧方式が途中でWork停止する場合の訪問数まで再現した値ではない。
後者は容量0・未知位相も含むので、物理NextDeparture省略数とは呼ばない。
物理位相計算の減少は、同条件の旧・新ランの `physical_calculations` の差で計測する。

## 回帰

最速直通、普通＋特急、10本の遅い直通、同点、11回乗換、低頻度特急と高頻度普通、
部分逆探索、Work不足、複数車両位相、同系統再乗車、貨物別容量・NoLoad/NoUnload・強制降車、
Scheduled Dispatchを比較する。既存の車庫・Waypoint・実乗降・Timed優先・Schedule Fallback回帰も実行する。

500種類のWork上限では旧方式349件、新方式446件を採用し、97件が追加で成立した。
旧方式が成立したケースの経路・費用は一致した。cold/warm、両キャッシュ無効、毎回破棄、
Load済みpeerでride/全route/費用/到着/乗換/Work/延期/理由/観測/labels/candidates/論理メモリを照合した。
Saveのbytesも一致し、Label/Memory admission境界も照合した。
旅客343 cases / 1,327,171 assertionsが成功した。同系統の別実車両へ30tickで再乗車し、
通常のthrough arrival 140tickを50tickへ改善するケースも明示的に確認した。
最終の計測オプションOFF製品構成ではCTest **556/556成功**。
初回構成は555件成功し、出力なしだったstationlist回帰は単独再実行で成功した。
最終構成の全件再実行は再試行なしで成功した。

## 同じ実マップ・2048tick×2組

MSVC 19.44 x64 Release `/MT`、assert ON、LTO OFF、null video/sound/music。
StateTicks **3176545487→3176547535** を旧/新/新/旧の順で逐次実行した。
公開末尾は旧方式649系統・3662車両、新方式648系統・3658車両。輸送の変更で後続公開も変化する。
固定資産は257 files、一覧SHA256は
`d33950cb0b9ee328fc92db2877f55e55232df2a762941c18a10d9fd0573f68b5`。
既存のNewGRF容量変更警告と、隔離した計測版のunpause処理を両方式で共通に使った。
ゲーム・ビルド・テストとの競合確認に引っかかったランはなく、速度測定を回帰・詳細測定と重ねていない。

| 指標 | 旧方式 | 枝刈り |
|---|---:|---:|
| 発車計算前に除外した乗車候補系統 | 0 | 1,259,960 |
| 未走査fleet候補 | 0 | 7,693,564 |
| 下界評価 / 停車Occurrence評価 | 0 | 5,453,583 / 12,882,455 |
| 物理位相計算 | 19,286,492 | 17,158,698 |
| 検索Work | 1,222,035,044 | 1,096,681,994 |
| Timed採用packets | 20,660 | 22,826 |
| Timed予算延期 | 15,503 | 13,469 |
| Work理由の結果件数 | 15,472 | 13,418 |
| Label理由の結果件数 | 31 | 51 |
| Memory理由の結果件数 | 0 | 0 |
| Timed実乗車 / 乗換 / 配送people | 12,240 / 2,515 / 1,846 | 13,511 / 3,075 / 2,319 |
| 実乗換後に次車両へ乗れたpeople | 1,005 | 1,071 |
| 前向き平均ms/tick（2回平均） | 13.099 | 12.079 |
| Journey平均ms/tick（2回平均） | 19.935 | 19.234 |
| simulation平均ms/tick（2回平均） | 41.841 | 41.355 |
| Journey p99 ms（2回） | 46 / 47 | 48 / 48 |
| simulation p99 ms（2回） | 71 / 71 | 78 / 72 |

前向きは7.79%、Journey全体は3.52%、simulation全体は1.16%短縮した。
物理位相計算の総数は2,127,794回減った。ただし採用・実輸送・後続照会も変わるため、
旧/新の差を「全く同じ照会を省いた回数」とは解釈しない。
Labelによる延期は増え、p99も改善していない。2組・一つのマップであり、一般的な速度保証にはしない。
次発索引や過去の8192tick報告とは負荷・区間が違うので、その値を今回の対照群に使っていない。

別の512tick詳細ランでは、下界評価の計測経過時間は **0.172ms/tick**、位相処理は
**0.899→0.743ms/tick**、差は **0.156ms/tick** だった。
この測定では下界評価の時間が省けた位相処理だけの時間を上回った。
降車先走査・ラベル挿入も省略することで前向き全体は改善したが、位相計算の節約だけでは説明できない。
詳細値はsimulation threadのsteady_clock scopeで、exclusive CPU時間ではなく、
多数のタイマー呼出しの観測コストも含む。詳細版を上の速度平均には混ぜていない。

各方式の2回のWork・結果・最終Saveはそれぞれ一致した。旧Saveは
`5ab4b68c2f69d35c52ee4022a12488e7a1ef3b410f6175a3af83e316675128b2`、
新Saveは `63addf09b06f687fb72d5303dd5cf76c5182739767855243785be2b6dcf87210`。
アルゴリズムが変わるため旧/新Saveの一致は要求しない。
新方式のpolicy/次発索引両cacheを無効にした同じ2048tickは、新方式のSave、Work、
全結果・延期理由・実輸送・枝刈り数と一致した。

[集計と全カウンター](measurements/passenger-timed-pruning-2026-10-13/summary.json)、
[旧方式ログ](measurements/passenger-timed-pruning-2026-10-13/baseline-1-console.txt)、
[枝刈りログ](measurements/passenger-timed-pruning-2026-10-13/pruned-1-console.txt)、
[キャッシュ無効ログ](measurements/passenger-timed-pruning-2026-10-13/cold-console.txt)、
[詳細ログ](measurements/passenger-timed-pruning-2026-10-13/detail-console.txt)。

## 同じ公開スナップショットの経路品質

新方式の末尾SaveをLoadし、StateTicksを3176547535のまま進めず、待機中のODを駅ID順に128件比較した。
明示的な比較用コピーconsoleは `Plan()` と `PlanWithoutServicePruning()` を直接呼び、
snapshot ServiceGraphや将来Visitを構築しない。Work 65536、Labels 8192、Memory 8MiB。
両方式とも126件を採用し、成立経路の喪失0、費用・到着・乗換回数・全routeの変更0だった。
Work合計は2,039,543→1,699,360（16.68%減）。これは一つの公開状態の、駅ID順に偏った標本であり、
全国OD全体の検証ではない。500種の小さいWork境界における追加採用97件は前記合成回帰で別途検証した。

[ODごとの結果と集計](measurements/passenger-timed-pruning-2026-10-13/quality-summary.json)。

## Save/Load・初期参加・途中参加

新方式の末尾Saveを私設127.0.0.1:17523サーバーでLoadし、初期参加と途中参加の2クライアントを接続した。
サーバーはcache有効、clientsはpolicy/次発索引/OD fallbackのcacheを全て無効にした。
最終の検証は通常のCPU割当て、sync_freq=1、私設試験のmax_lag_time/max_join_time=32000。
StateTicksは3176547535→3176548402の867tick。初期clientは同じ開始tickから、
途中clientは3176547858から進行し、**両方が3176548402に追従したことを記録**した。
末尾に2クライアントが接続中で、同期不一致・assertion・切断の検出0。
client側にも実際の枝刈りがあり、policy/次発索引/OD fallbackのhitは0だった。

進行確認用のnull driverコピーに、Normal network clientのみ128tickごとの読み取り診断を加えた。
ルーティング翻訳単位は性能測定と同じソースSHAである。network用コピーは速度比較に使わない。
初回の1コア・全cache無効ではgeneral timeoutになり、継承したharnessはjoin件数だけで誤って成功を返したが、
終了時の接続ログを読み、成功扱いを撤回した。
進行記録の日単位scheduleは未対応、月単位はこの区間で発火しなかったため、どちらも成功証拠に使わない。
128tick probeを付けた1コア試験も追従待機上限に達した。
通常CPUの最終試験だけを成功とし、1コア・cache全無効で実マップに追従する性能は保証しない。
有限のloopback試験であり、WANや長時間運用の保証ではない。

[結果](measurements/passenger-timed-pruning-2026-10-13/mp-result.json)、
[サーバー](measurements/passenger-timed-pruning-2026-10-13/mp-server-console.txt)、
[初期clientの実マップ進行](measurements/passenger-timed-pruning-2026-10-13/mp-client-initial-progress-console.txt)、
[途中clientの実マップ進行](measurements/passenger-timed-pruning-2026-10-13/mp-client-late-progress-console.txt)、
[ソース・バイナリ識別と失敗試験の区別](measurements/passenger-timed-pruning-2026-10-13/verification-metadata.json)。

## 再現

`build*` のSave/NewGRF/exeはコミットしない。実マップは既存のverified-result.savと同じ
SHA256 `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390` を使う。
既存のProfile runnerで固定したpackageに、今回のbaseset/langと展開したOpenGFXを重ねて固定する。
全比較ランで同じpackageを使う。プロセス競合のあるランは性能結果に採用しない。

```powershell
cmake -S . -B build-timed-pruning -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=PATH/TO/x64-windows-static" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DOPTION_NO_LTO=ON -DOPTION_USE_ASSERTS=ON -DOPTION_PASSENGER_BENCHMARK=OFF
cmake --build build-timed-pruning --config Release --target openttd openttd_test regression_files --parallel 8
python docs/measurements/passenger-routing-profile-2026-10-13/Run-Profile.py freeze --package build-timed-pruning/package --save PATH/TO/verified-result.sav --config PATH/TO/openttd.cfg --assets PATH/TO/OpenTTD-data --build build-timed-pruning
python docs/measurements/passenger-timed-pruning-2026-10-13/Prepare-Assets.py build-timed-pruning
ctest --test-dir build-timed-pruning -C Release --output-on-failure -j 4
& ./build-timed-pruning/Release/openttd_test.exe '[passenger]' --reporter compact
& ./docs/measurements/passenger-timed-pruning-2026-10-13/Build-Comparisons.ps1 -Source . -Build build-timed-pruning -Dependencies PATH/TO/x64-windows-static
python docs/measurements/passenger-timed-pruning-2026-10-13/Run-Comparison.py build-timed-pruning --ticks 2048 --detail-ticks 512
python docs/measurements/passenger-timed-pruning-2026-10-13/Summarize-Comparison.py build-timed-pruning/comparison docs/measurements/passenger-timed-pruning-2026-10-13
```

比較ビルドはコピーした翻訳単位だけをVS projectへ接続する。baseline/prunedは共通のconsole量子化出力のみ、
詳細版は別途bound/phase scopeのsteady_clockタイマーを加える。詳細版を速度比較へ混ぜない。
PE subsystemをGUIからconsoleへ変えるが、コードは変更しない。
計測runnerはPowerShell 7で起動する。今回の最初のWindows PowerShell 5ランは、
ゲーム完走後にGet-FileHashの解決に失敗したため速度比較から除外し、同じ入力で全比較をやり直した。

経路品質比較はfresh configureの後に `Prepare-Comparison.py . build-timed-pruning --mode quality` を適用して
`openttd` を構築し、`Run-Quality.ps1` に既知の末尾Saveと固定資産のrun directoryを渡す。
このコピーの `passenger_periodic_compare sample 128` はpruned/unprunedだけを比較する。

ネットワーク検証はfresh configure（OPTION_PASSENGER_BENCHMARK=ON）の後に
`Prepare-Network-Probe.py . build-timed-pruning` を適用して `openttd` を構築し、console subsystemのコピーを作る。
`Prepare-Multiplayer.py build-timed-pruning --directory mp-pruning-unrestricted --probe --unrestricted-clients` で固定資産とharnessを作る。
生成した `Exercise-Pruning-Multiplayer.py` に、末尾Save/exeのあるrun、上記network directory、
`--binary openttd-network.exe --backend periodic --input-save pruned-1-result.sav --duration 25` を渡す。
これは速度測定とは別に実行する。

[旅客回帰](measurements/passenger-timed-pruning-2026-10-13/passenger-tests.log)、
[全製品CTest](measurements/passenger-timed-pruning-2026-10-13/ctest-product.log)。
