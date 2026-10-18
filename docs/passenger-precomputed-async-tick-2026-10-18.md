# 旅客事前計算表の非同期Workerと通常tick

基点は `sjcl/perf/passenger-precomputed-parallel-build` の `8c5db92b90`。
作業ブランチは `perf/passenger-precomputed-async-tick`。

## 実装

Legacyの逆向きDijkstraとHyperpathのイベント探索を、プロセス内で再利用するWorkerへ移した。
初期構築と通常の世代更新で同じ非同期先行探索を使う。
`StepParallel()`、`TickPrecomputed()`、テーブルのreset・破棄からWorkerをjoinする経路はない。
joinはプロセス終了時のプール停止だけに残る。

探索の入力は、直接構築した `GraphSnapshot` を `shared_ptr<const GraphSnapshot>` で共有する。
Worker用のグラフコピーは作らない。
Workerはテーブル、ライブ駅・車両、サービスのポインターを持たない。
探索の診断カウンターもWorkspace内にあり、テーブルの可変診断・Policyキャッシュに触れない。
サービスのbinding更新はメイン側の別領域へ置き、確定したグラフを変更しない。
結果は世代ID、目的地index、StationIDを持ち、release/acquireで完成した結果だけを受け渡す。

世代の所有権を放すとキャンセルを通知する。
Workerは4096 Work以下の探索単位の間でキャンセルを確認する。
実行中のジョブはグラフと結果の所有権を持つため、旧テーブルをmove・reset・破棄しても参照が失効しない。
待機中のWorkerもキャンセルを確認し、不要なWorkspaceとグラフを解放する。
破棄処理からWorkerの停止完了を待たない。

## 決定的なWorkと公開

Workerはローカルな計算キャッシュであり、ゲームの進捗を決めない。
メインスレッドは従来どおり65,536 Work/tickを消費する。

各Rowの探索開始時に完成済みの結果があれば、その探索Workを仮想的に消費する。
探索Workの終端に達してからWorkspaceを採用し、従来のRow完成処理を続ける。
未完成・失敗・資源不足なら、そのRowは最初から最後まで逐次探索する。
その後で完成した結果には切り替えない。
Workerを待つためにWorkや公開を次tickへ送る処理はない。

目的地順、代替候補抽出、Policy圧縮、Label・候補のコピー、到達先逆引き、乗車マスク、公開callback、旧Row退役はメイン側の既存state machineが担当する。
Worker数と完了順序は、候補集合、公開Work、論理メモリ入場判定に影響しない。
Worker数0は逐次経路を使う。
仮想Workの途中で逐次 `Step()` へ戻す場合は、消費済みprefixを再探索してから続ける。prefixのWorkは再加算しない。

未公開結果、ジョブ、スレッド、ローカル診断は保存しない。
保存する総Workと既存の逐次 `StepLoad()` による再生を維持した。
JPSD、保存feature、ネットワークプロトコル、参加絶対タイムアウトは変更していない。

## メインスレッドに残る処理

サービス発見・スナップショット、グラフ構築、論理メモリ入場判定、Row完成・公開、旧Row退役、経路照会はメイン側に残る。
製品のSimulationは既存の `Tick(..., prepare_graphs=true)` を使い、グラフ構築をcooperativeな準備バリアで分割する。
このバリア中はStateTicksを進めず、UI・ネットワークへ戻る。
置換グラフが完成したら、0 Workの呼び出しでWorkerを起動し、通常tickの開始前から先行探索できる。
グラフ構築のCPU自体はバックグラウンドへ移していない。
直接の `TickPrecomputed()` 呼び出しで準備バリアを省略した場合は、従来と同じ同期グラフ構築になる。

## ローカル資源

`gui.passenger_precomputed_workers` は既存のローカル設定のまま、0〜8、既定4。
プールはCPU数と要求数で制限し、全テーブルを通じて最大8スレッドを再利用する。
各世代の未採用結果は採用途中のRowも含め最大2×Worker数、共有キューは最大16ジョブ。
WorkerごとのWorkspace、queue、探索配列は同じ世代のジョブ間で再利用する。

追加scratchは、状態数・辺数から保守的に推計したWorkspace単価で予約する。
未採用結果、実行中のキャンセル済みジョブ、Workerの作業領域を合わせて4×Worker数の単価を予約し、全世代で512 MiBを上限とする。
これは予約推計の上限であり、OSが報告するプロセス全体のメモリ上限ではない。
既存のグラフ、公開Row、保持Row、アロケータ管理領域は別に存在する。
予約不足ならWorker数を減らす。スレッド起動の一部に失敗しても起動済みの範囲で使い、0本なら逐次処理へ戻る。
追加確保・探索に失敗した世代も逐次処理へ戻る。
論理的な経路表のメモリ拒否にはこのローカル判定を使わない。
16 Row連続で完成済み結果を使えなければ、その世代の先行計算をキャンセルする。
これは遅いWorkerによる重複CPU消費を抑えるローカルな制御であり、Workや公開状態を変えない。

## 検証・計測手順

通常テストでは独立した旧逐次エンジンとの比較を維持し、各公開境界のLabel全フィールド、Policy、候補、逆引き、乗車マスクを比較する。
遅延Rowの逐次処理後の結果破棄、逆順完了、探索途中の例外、move・reset中の旧グラフ生存も対象にする。
DBでは0対8、8対0、4対1、片側遅延、片側失敗で保存バイト列と新旧Rowを比較し、途中参加の逐次再生を行う。

`utils/passenger_parallel_benchmark.py` は各Worker数を別プロセスで実行する。
`--scenario initial` は初期構築、`natural` は連続する通常tick、`ahead` はtick外でWorker先行を確保する対照実験、`late` はWorkerを1量子後で留める対照実験。
待機フックはテストだけに存在し、ゲームWorkには加算しない。
`ahead` のtick外の待機もwall時間には含め、TickPrecomputed内の経過時間からは分離する。
メインCPUは短区間ごとの計測誤差を避けるためループ全体で測り、対照実験の制御処理のCPUも含む。
置換グラフの構築は別に計測し、その後の通常TickPrecomputedは従来の65,536 Workを使う。
`--uneven` は大きな連結成分に32個の独立した2駅系統を加え、目的地ごとの探索負荷を変える。

メインとWorkerのCPUはネイティブのスレッドCPU時計で計測する。
WindowsではスレッドHANDLEを保持し、メインからWorkerの累積CPU時間を読む。
CPU時計の分解能は粗く、短い処理では0や約15.6 ms単位の値になる。
キャンセル後の25 msの安定待ちは、報告する構築・tick経過時間の外に置く。
`main_search_ns` は探索区間の経過時間で、スレッドCPU時間とは区別する。
スレッド起動数、Workspace再利用数、採用率、逐次Row数、未使用探索Work、ピークメモリも記録する。
Worker結果待機時間は製品の呼び出し経路に待機処理がないため0。

ループバック試験は `utils/passenger_async_probe_prepare.py` でコピーした計測用TUだけに遅延・失敗・headstartフックを挿入する。
vanilla fixtureに既知ODのPacketを投入し、通常の保存・乗車・配送・支払い処理を両peerで進める。
製品ソースに環境変数のフックやネットワーク例外は追加しない。

## 最終検証

MSVCのRelease、assert有効で全741テスト、10,586,458アサーションが成功した。
旧逐次エンジン `093218f882` を別TUでビルドした参照比較も含む。
完全な記録は [native-tests.log](measurements/passenger-async-tick-2026-10-18/native-tests.log)。

追加の実輸送テストでは40目的地の世代更新を0、8、高遅延4、探索途中失敗4 Workerで再現した。
Legacy・Hyperpathの両方で、各Work境界のDB保存バイト列と、実際の2車両への乗車・乗換・配送を比較した。
Packetを乗換途中で保存・再読込し、Intentを含む全保存フィールド、所属リスト、経由地、乗車可否、Transfer・Deliverの判定が一致した。
時計・遅延フックをWorkへ混ぜず、Workerを留めたままメインが最後まで完了するテストも成功した。

## 合成ベンチマーク

AMD Ryzen 9 9950X（16コア・32論理CPU）、Windows 11、MSVC 19.44.35207 x64。
Release `/O2 /Ob2`、assert有効、dedicated、探索TUの`/GL`なし。
各条件を新しいプロセスで3回実行し、以下は中央値。合計150試行。
[集計JSON](measurements/passenger-async-tick-2026-10-18/benchmark-summary.json)に生値、SHA、CPU・メモリ・Work・digestを記録した。

894駅、240系統の初期構築。wall・メインCPUの単位はms、ピークはプロセスWorking SetのMiB。
メインCPUにはグラフ準備とRow完成処理も含む。

| Worker | Legacy wall | Legacy main CPU | Legacy peak | Hyperpath wall | Hyperpath main CPU | Hyperpath peak |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 882.87 | 890.63 | 120.20 | 924.66 | 937.50 | 213.41 |
| 1 | 872.81 | 875.00 | 120.51 | 883.27 | 890.63 | 214.04 |
| 2 | 590.81 | 593.75 | 125.54 | 516.72 | 515.63 | 225.80 |
| 4 | 412.44 | 406.25 | 130.76 | 283.24 | 281.25 | 230.78 |
| 8 | 351.07 | 359.38 | 145.08 | 219.12 | 203.13 | 244.30 |

8 Workerで初期wallはLegacy約2.51倍、Hyperpath約4.22倍の速度になった。
実際のスレッド生成はそれぞれ0、1、2、4、8回。バッチごとの生成・joinは0回。
894 Rowを処理してもWorkerは増殖せず、8 WorkerのWorkspace再利用はLegacy846回、Hyperpath868回だった。
初期探索区間のメイン経過時間はLegacy564.71→11.94 ms、Hyperpath765.68→3.45 ms。

通常更新はグラフ準備後に65,536 Workで`TickPrecomputed()`を連続実行した。
以下のtick時間は世代全体の合計で、UI待ちやフレーム間の余白を入れない負荷試験である。

| Worker | Legacy tick合計 ms | Legacy main CPU ms | Legacy採用率 | Hyperpath tick合計 ms | Hyperpath main CPU ms | Hyperpath採用率 |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 630.37 | 625.00 | — | 850.31 | 843.75 | — |
| 1 | 652.43 | 640.63 | 0.1% | 846.23 | 843.75 | 0.2% |
| 2 | 358.39 | 359.38 | 58.6% | 529.91 | 500.00 | 53.4% |
| 4 | 216.96 | 218.75 | 82.7% | 284.29 | 265.63 | 83.1% |
| 8 | 152.75 | 156.25 | 94.7% | 173.03 | 171.88 | 99.4% |

通常更新の8 Workerで、メインCPUはLegacy75%、Hyperpath約80%減少した。
探索区間のメイン経過時間は553.10→54.80 ms、742.73→8.13 ms。
論理tick数は全Worker数でLegacy1092、Hyperpath1027。
論理Workはそれぞれ71,561,446、67,301,884で同じだった。
各tickのWork・公開Row数・保持Row数のdigestと、最終Row全フィールド・DB保存digestも一致した。

8 Workerの通常tick経過時間は、中央値がLegacy97.2 µs、Hyperpath159.1 µs。
p95は539.4 µs、268.5 µsで、逐次版の685.8 µs、1059.2 µsから下がった。
試行ごとの最大値の中央値は3.78 ms、5.18 msで、逐次版の2.12 ms、2.40 msより大きい。
OSスケジューリング・割当・Row公開の瞬間負荷は残り、最大tickの短縮まで保証する変更ではない。
全条件でWorker結果待ちの時間は0、通常tickでjoinや完了待ちを行わない。

### 常時先行・常時遅延

| 条件（894駅） | Legacy tick合計 ms | Legacy main CPU ms | Hyperpath tick合計 ms | Hyperpath main CPU ms | 採用率 |
|---|---:|---:|---:|---:|---:|
| ahead 0 | 636.39 | 625.00 | 859.06 | 828.13 | — |
| ahead 8 | 225.31 | 234.38 | 423.19 | 375.00 | 両方100% |
| late 0 | 639.45 | 640.63 | 833.01 | 828.13 | — |
| late 8 | 655.86 | 640.63 | 848.56 | 843.75 | 両方0% |

aheadはRow開始前のtick外で完成を保証する対照実験で、メインの探索区間は両方0だった。
制御のsleepとOSの待機粒度のため、世代全体のwallは約13.6秒になる。
これは製品の待機でも実運用の構築時間でもなく、採用時に探索CPUが消えることを確認する測定である。
メインCPUは制御処理まで含む保守的な値であり、aheadのwallを高速化率には使わない。

lateは各Workerを最初の4096 Workの後で留め、メイン完了後に解放した。
全894 Rowが逐次で完成し、遅れた結果へ切り替えず、Work・保存・Rowは逐次版と一致した。
16連続missで先行計算を停止したため、重複探索は合計32,768 Workに留まった。
1 Workerの連続tick試験も先行できず同様に停止し、CPU時間は概ね逐次版へ戻った。

### 不均一な目的地負荷

958駅の入力は894駅の大成分と32個の独立2駅成分で構成した。
完成した目的地探索のWork範囲はLegacy約11,859〜70,388、Hyperpath約10,259〜63,293で、約6倍の差を持つ。
均一入力の小さい差とは異なる負荷でも、全条件のWork・公開状態・保存digestが一致した。

| 958駅・通常更新 | Legacy 0→8 | Hyperpath 0→8 |
|---|---:|---:|
| tick合計 ms | 638.98→135.80 | 863.95→182.49 |
| main CPU ms | 625.00→125.00 | 859.38→171.88 |
| 8 Worker採用率 | 97.0% | 99.5% |
| peak Working Set MiB | 147.74→169.87 | 249.85→285.06 |
| 論理tick数（0と8で同一） | 1120 | 1053 |

初期構築の958駅wallは0→8でLegacy884.76→347.54 ms、Hyperpath889.20→214.37 ms。
958駅のaheadは958 Rowすべてを採用し、lateはすべて逐次で処理した。

### CPU・メモリの解釈

894駅の通常8 Workerの平均稼働率推計はLegacy約90%、Hyperpath約88%。
Worker CPUをWorker数×世代処理wallで割った概算であり、CPU時計の量子化と別々の中央値の比による誤差を含む。
2〜4 Workerでは遅れたRowの二重計算が多い。
通常更新の未使用探索WorkはLegacy2 Workerで約21.3百万、8 Workerで約2.77百万、Hyperpathで約21.4百万、約0.23百万だった。
8 Workerのメイン＋Worker CPUは約1.25秒、1.39秒で、逐次の約0.63秒、0.84秒より増える。
今回達成したのはメインCPUの削減であり、総CPUや電力の削減ではない。

通常894駅のピークWorking Setは0→8でLegacy134.63→156.94 MiB、Hyperpath230.29→261.45 MiB。
ピークCommitは128.54→150.19 MiB、224.72→255.76 MiB。
これらは測定前の旧世代、保存digest、ランタイムも含むプロセス全体の最大値。
8 Workerのscratch予約推計はLegacy約126.5 MiB、Hyperpath約100.6 MiBで、実測使用量とは異なる。
診断`bytes`にはローカルWorkspaceのcapacityも入り、Worker再利用で値が変わる。
論理的なenvelopeと公開Rowの`bytes`を含む全フィールドは一致している。

グラフ準備はLegacy約209〜214 ms、Hyperpath約2〜3 msのメイン経過時間を要する。
製品はこれを従来のcooperative準備処理で分割するが、CPU負荷自体は残る。
ほかにLegacyの代替候補生成、HyperpathのPolicy圧縮、公開用配列コピー、乗車マスクと旧Row退役が残るボトルネック。
ThreadSanitizerはこのWindows環境で実行しておらず、競合の検証は所有権・同期のコード確認と意図的な遅延・逆順・破棄テストによる。

## ホスト・クライアント非対称MP

最終版の計測用バイナリSHA-256は`763bfd70538e69d96c9c77ce6125d6b84507e419f1a6b9824a1d51b733a1f1d3`。
同一バイナリの別プロセスをループバック接続し、Legacy・Hyperpathそれぞれで以下の5条件、計10条件が成功した。
[MP全結果](measurements/passenger-async-tick-2026-10-18/multiplayer/matrix.json)に両peerのスナップショットと比較結果を収録した。

| ホスト設定 | クライアント設定 | 採用Row（ホスト／クライアント） | 結果 |
|---|---|---:|---|
| 8、高速 | 0、逐次 | 3／0 | 両モード一致 |
| 0、逐次 | 8、高速 | 0／3 | 両モード一致 |
| 4、高速 | 1、各量子5 ms遅延 | 3／0 | 両モード一致 |
| 4、正常 | 4、途中例外 | 3／0 | 両モード一致、失敗1回から逐次へ |
| 4、各量子5 ms遅延 | 4、高速 | 0／3 | 両モード一致 |

高速側にはテスト用のメイン探索headstartを入れ、片側だけ完成済みキャッシュを使うことを診断で確認した。
Worker設定は8でも実fixtureの有効目的地は3つなので、このループバックで同時に動くWorkerは最大3本。
完全な8本と多数のtickにまたがる公開境界は、40駅のネイティブ比較と894／958駅の合成試験で検証した。
この小さいfixtureの参加時再生は短く、進捗通信は各条件1件だった。
長い途中Row・旧世代保持中の再生は、任意Work量子で保存バイト列を比較するネイティブ試験で検証した。

初期構築、途中参加の保存再生、逐次から事前計算への切替、世代更新と実輸送を同じセッションで実行した。
比較時点は各ケース内で同じStateTicksで、初期表のWorkはLegacy197、Hyperpath266、公開Rowは3で一致。
全Rowフィールド・Policy・候補・保持Rowのdigest、DB保存digest、Packet全状態・所属・経由地・Intent、乗車状態、支払い・会社資金・車両収益も一致した。
要求2048 tick以上、実測2088〜2090 tick進め、各peerで乗車67人・配送52人を確認した。
このfixtureでは乗換は0件のため、非ゼロの乗換と途中Packet保存は前述の2車両ネイティブ試験で補った。
desync・network errorは0、既存の参加タイムアウト設定を変更しなかった。
同一セーブでの長時間実マップ試験は今回実施していない。

## commitとpush先

1. `ad839eec76` 常駐プールとWorkspace再利用
2. `669602d48a` 有界ジョブ投入・非ブロッキング回収
3. `ac0098c46c` immutableグラフと世代所有権
4. `28ed7584bf` 初期バリアへの非同期適用
5. `581644111b` 通常tickへの先行探索適用
6. `e26c89add0` Row単位の逐次固定とキャンセル
7. `6c6cc22fa2` 保存・異速度MP・全フィールド比較テスト
8. 本レポートを含む最終commit：150試行、追加輸送テスト、16 missのCPU抑制と通知負荷削減

push先：`sjcl/OpenTTD-patches` の [`perf/passenger-precomputed-async-tick`](https://github.com/sjcl/OpenTTD-patches/tree/perf/passenger-precomputed-async-tick)。

## jgrpp-yaへの統合検証（2026-10-18）

`jgrpp-ya`の`5a00c36276`へ、非同期Workerブランチの`043755aed4`を統合した。
駅サービスの段階表示と経路オーバーレイの変更を維持し、表示用の2関数をimmutableな`GraphSnapshot`へ対応させた。
段階表示は`FindMember()`を通じて現在のサービス世代を読み、Workerのスナップショットに保存された旧bindingを表示しない。
このbinding更新の回帰テストも追加した。

メイン探索、Worker、Work、Row公開・退役、保存に関する実装は、検証済み非同期Workerブランチと同じであることを確認した。
MSVC Release・assert有効でゲーム本体とテストをビルドし、参照逐次エンジンを含む全763テスト、10,587,639アサーションが成功した。
[統合後のテスト出力](measurements/passenger-async-tick-2026-10-18/integration-tests.log)を収録した。
上記の150試行とMP 10条件の測定値は、非同期Workerブランチで記録した値である。
