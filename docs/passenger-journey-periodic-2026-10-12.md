# Journey周期バックエンド：実装と測定（2026-10-12～13）

ベースは `3373f64f8b68d57726f86c4b5ccfb7a236099b61`、作業ブランチは
`feat/passenger-journey-periodic-backend`。長寿命バックエンドをActiveに接続し、
実マップの通常運転でOD別ServiceGraph構築・物理Trip予測を0回にした。
**Journey平均5ms/tickの目標は未達**。最終の継続区間では13.623ms/tickだった。
ゲーム全体27ms/tickも達成していない。

## 実装した範囲

Phase 0～6の主要経路を実装した。詳細な形式・上限は
[バックエンドガイド](passenger-journey-periodic.md)を参照。

* OrderListIDごとの所有データ、保存されたincarnationと公開世代、駅別occurrence索引。
* 一周分のTravel/Waitフラグ付き周期ダイヤ。明示的0と未設定0を区別し、
  チェック付き周期演算で次便を求める。共有車両の位相は実車ごとに保持する。
* 条件を証明できる単独実車のDispatchスロット。競合・未対応制御はUntimed扱い。
* 最大8個のActive経路候補を含む長寿命ODポリシー。絶対時刻の勝者を保存せず、
  評価時に各候補の次便を再計算する。未対応の目的駅・位相・接続成分は安価に除外する。
* `VehicleSnapshot` / `ServiceIntent` / `BoundService`の区別。実乗車時に実車、
  指令、施設、乗降条件、降車occurrenceを検証し、既存の空席・予約・支払い処理へ渡す。
* シミュレーションスレッド上の固定65,536 Work/tick構築、保存される走査位置と回転Dirtyキュー。
  時間だけの更新は1,024tickの決定的な周期でまとめる。更新が続いても最初の待機時刻を保持する。
* 実到着・便の失効・再試行・世代変更による再評価。同じイベントの同一ODは結果を共有する。
* 構造・辺の利用可否・駅とnodeの対応を検証する長寿命Fallback距離と共有逆向きCSR索引。
* `JPSD`による公開データ・位相・世代・更新進捗の保存。Cargo実行拡張はversion 2。
  ODポリシーとFallback配列は保存せず、ヒット時もコールド時の論理Workを同じように支払う。

`linkgraph.passenger_journey_periodic=false`で比較用の旧Active経路を選べる。
旧保存JourneyはVehicleIDを要求する旧契約のまま扱う。
Train/RoadVehicle/Ship/Aircraftの実予約・分割・段階積載・最終OD配送は回帰テストで確認した。

## 測定方法

同じ実セーブ・同じ資産を隔離した作業ディレクトリにコピーして使用した。
入力SHA256は `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`。
地図は16,384×4,096。初期待機人数は200,450人。
元コミットを別ディレクトリへ展開し、
[観測用パッチ](measurements/passenger-journey-periodic-2026-10-12/baseline-instrumentation.patch)
だけを加えた比較バイナリをビルドした。元の経路アルゴリズムは変更していない。

MSVC 19.44 / VS2022 / Release / static vcpkg依存 / assertions有効 / LTO無効。
実マップの比較は逐次実行し、ビルド・テスト・別ゲームを重ねていない。
Journey以外の負荷を分離するため、Journey無効・OD assignment有効の元コードも測定した。
`recalc_interval=8`、`recalc_time=32`、旅客分配はAsymmetric。

主比較はstate ticks `3176545487→3176547014`。
月境界の計測出力時点で完了したtick標本は1,526個。
初期区間は最初の128tickを別プロセスで測定した。
継続区間は新方式の月境界セーブをロードし、設定コールバックを再実行せず次の月まで進めた
2,219標本であり、主比較より後の期間である。初期区間は主比較の部分区間で、別条件の優劣比較には使わない。

時間は壁時計による観測で、Work admission・公開タイミング・保存内容には使わない。
Journey時間はネストした呼出しの最外側だけを積算する。
p95/p99は1msヒストグラムの上限値で、精密な百分位値ではない。
最後の2,047ms以上のbinはoverflow用だが、今回の最大値はそれ未満。
単回測定であり、ノイズや経路差を含む。

元セーブはロード時からNewGRFの容量変更警告を出すため、隔離した測定ビルドだけ
`OPTION_PASSENGER_BENCHMARK=ON`で既存のError pauseから継続させた。
製品ビルドの既定値はOFF。この処置と固定tick終了時の診断出力は測定用の処理である。

## tick時間

単位はms。百分位欄は上限値。

| 条件 | 標本 | 全体平均 | p95 | p99 | 最大 | Journey平均 | p95 | p99 | 最大 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 元コード・Journey無効 | 1,526 | 25.258 | 35 | 42 | 65.326 | 0.000※ | 1 | 1 | 0.038 |
| 元コード・Journey有効 | 1,526 | 289.048 | 367 | 412 | 492.046 | 266.096 | 339 | 382 | 454.199 |
| 新方式・初期128tick | 128 | 37.844 | 49 | 54 | 59.381 | 14.694 | 24 | 26 | 31.072 |
| 新方式・同じ主比較区間 | 1,526 | 38.876 | 53 | 62 | 99.646 | 13.599 | 25 | 29 | 38.918 |
| 新方式・ロード後の継続区間 | 2,219 | 39.151 | 54 | 62 | 80.211 | 13.623 | 26 | 30 | 41.687 |

※無効時のJourney平均は整数us表示で0へ切り捨てられた。
新方式の主比較Journey平均は元方式より約94.9%減少した。
以前の234msという測定値を今回の比較値として再利用していない。
途中の試作で3.898msが出たが、頻繁なDB破棄で全国の周期データがそろわない状態だったため、
それを最終成果や5ms達成の証拠として扱わない。

## 処理内訳と輸送

| 主比較区間の指標 | 元コード | 新方式 |
|---|---:|---:|
| ServiceGraph構築 | 23,409回 / 294.358秒 | 0回 / 0秒 |
| 物理Trip予測実行 | 29,006,966回 / predictor 120.864秒 | 0回 |
| DepartureIndex構築時間 | 42.368秒 | 0秒 |
| Planner / 周期OD検索 | 22,783回 / 20.099秒 | 24,416回 / 14.683秒 |
| NextDeparture参照 | — | 222,601回 |
| 長寿命ODポリシーヒット / ミス | — | 9,015 / 15,401（36.9%） |
| DB公開レコード構築 | — | 4,028回 / 303,080us |
| 末尾公開DB | — | 837系統、時刻既知509系統、2,835車両、5,354,064byte |
| 全体無効化 | — | 3回 |
| 末尾Pending / 観測最大待機 | — | 558件 / 702tick |
| 末尾ポリシー / Fallback共有キャッシュ | — | 105,088 / 16,872,236byte |
| Fallback探索 / 論理Work | 旧ログ参照 | 52,944回 / 2,541,550,847 |
| Journey適用 | 12,423packet / 30,781人 | 4,003packet / 8,061人 |
| 時刻付き：実乗車 / 乗換 / 配送 | 11,226 / 21 / 2,371人 | 2,693 / 0 / 645人 |
| Untimed：実乗車 / 乗換 / 配送 | 78,059 / 39,653 / 29,981人 | 84,552 / 38,879 / 31,357人 |
| 配送合計 | 32,352人 | 32,002人 |
| 末尾待機：時刻付き / CargoDist・延期 / OD fallback等 | 3,688 / 166,736 / 40,659人 | 1,681 / 192,403 / 17,931人 |

旧Trip予測・Graph各phaseは包含関係があるため時間を足し合わせない。
新方式もwaiting=15.043秒、fallback distance=5.202秒、boarding=4.142秒は包含区間である。
Workはヒット時も支払う論理量で、実CPU時間とは異なる。
時刻付き輸送だけを見て輸送停止や精度改善を断言できない。
未知Dispatch・候補上限・異なる予定便による意図的な差と、ライブ乗車の安全性を区別する必要がある。

新方式の継続区間でもGraph・物理予測は0回。
NextDeparture 237,922回、ポリシーヒット14,120 / ミス21,384（39.8%）、
DB更新441,983us、Pending626件、最大観測待機687tick。
時刻付き乗車4,065人・配送2,830人、Untimed乗車147,822人・配送60,497人だった。

プロセスpeak working setは元無効約1,876MiB、元有効約1,909MiB、
新主比較約1,907MiB、新継続約1,919MiB。100ms間隔のsamplingであり、
private memoryの短いpeakやallocatorの厳密な最大値を保証しない。
資産・地形・他サブシステムを含む値である。元の2測定のexit codeは収集実装の不備でnull、
成功ログと出力セーブは存在する。新方式の測定exit codeはすべて0。

測定後、駅索引のtree node・sentinel・vector slackを十分に見込むよう、
stopごとのretention見積りを192→512byteへ保守化した。データ構造を増やす変更ではない。
上表のDB byteは測定時の推定値で、製品構成ではより大きい見積りになる。

## 未達と制限

5ms目標を超える主因は、更新後のOD候補探索とFallbackである。
主比較の周期OD検索だけで約9.62ms/tick、DB構築は約0.20ms/tickだった。
ヒット率は約37～40%にとどまり、局所更新・公開時にも現在はODポリシー全体を消す。
候補のnominal costはダイヤ変更で変わるため、単に古い候補を残すと
ロード後のコールド再計算と結果が一致しない。この点を省略して高速化していない。

* 最大8候補の保持は大規模網全体の最適性の証明ではない。
* 未対応制御の周期内で安全な区間だけを部分コンパイルする機能は未実装。
* 混雑・時刻窓容量providerによる周期評価は未実装。要求は明示的に延期し、旧planner APIは保持する。
  Activeの容量上限確認と実座席割当は実装済み。
* 一系統の構築は原子的で、巨大系統の内部まで複数tickに分割しない。
  上限超過系統はUntimedとして公開する。全国構築は固定Workで分割する。
* 全体破棄は即時で、分割garbage collectionは未実装。
* 保存時には旧DB・wire image・復元DBの一時重複があり、64MiB resident envelopeとは別の一時量を要する。
* 実マップの途中参加同期には後述の既存再現問題が残る。

## 証拠と再実行

[集計JSON](measurements/passenger-journey-periodic-2026-10-12/summary.json)と各`*-console.txt`、
`*-process.json`に数値を残した。大量の既存NewGRF警告は抜粋と全文SHA256・行数を保存し、
全文は作業ディレクトリに保持している。実セーブ・資産・実行ファイルはGitへ追加していない。
試作結果も残し、最終の`startup-final` / `periodic-final` / `steady-final`と区別した。

[測定スクリプト](measurements/passenger-journey-periodic-2026-10-12/Measure-Periodic.ps1)は
対応バイナリ・資産を用意したRunDirectoryを受け取る。
旧バイナリには`-Original`、初期128tickには`-Ticks 128`、
継続測定には`-Resume -InputSave <直前の結果セーブ>`を使う。
比較バイナリごとに、そのソースで生成したlanguage packを使う。

## 最終回帰と同期

初回実装時は`OPTION_PASSENGER_BENCHMARK=OFF`の製品構成でビルドし、
**CTest 475件すべて成功**、`[passenger]` **262ケース / 966,155 assertions成功**。
周期関連は43ケースで、無限繰返し、異周期乗換、並行便、個別共有位相、Dispatch競合、
明示/未設定0、条件/Depot/Refit/Unbunching、遅延、暗黙停車、プールID再利用、
局所更新・更新の重複・保存された進捗、コールド/ウォーム、負のtick境界、
8,000駅/4,000車両、同一OD4,096packet、各車種の実予約・分割・最終OD配送を含む。
既存の無効時動作、旧/current save、GUI・診断・save purityの製品回帰も成功した。
Windowsのフォント登録が制限される実行環境では既存の文字出力回帰が環境警告で失敗したため、
最終チェックは通常のMSVC/Windows実行環境で行った。golden出力は変更していない。

初回の実マップ検証では、初期参加の後に途中参加したクライアントが**再開1tick後のSTATION乱数不一致**を検出した。
元コミットの比較バイナリでも同じ形の不一致を再現した。
[新方式の抜粋](measurements/passenger-journey-periodic-2026-10-12/mp-real-periodic-desync-excerpt.txt)と
[元方式の抜粋](measurements/passenger-journey-periodic-2026-10-12/mp-real-original-desync-excerpt.txt)を残した。
これだけで原因をNewGRFに断定したり、新方式の全同期条件を証明したりはできない。
この時点では、この実セーブで途中参加が成功することは未確認だった。

### 10月13日：CPU条件を揃えた再現と修正

公開リリース`jgrpp-ya-0.73.3+2`（コミット`9a3484bc78750e6ae1df5d58bba36761fe23d948`）と
現行コミット`7d914aa6f2`を区別して再検証した。元の測定セーブは`linkgraph_route_penalty=2`のため、
上限1の公開リリースでは読み込めない。ユーザーが同リリースで保存した別の実セーブ
（SHA256 `abc6c1ebdd48da042cc65159e0d57db5d89a88330e897bd590f2f00c3a80a276`）では、
公開Windows x64版が3,754tick・途中参加後90秒の同期を維持した。
現行版でも同じ提供セーブ・CPU制限なしで3,692tick・途中参加後90秒の同期を維持した。
1コア制限の公開版試行はタイムアウトであり、同期成功の根拠には含めない。

CPU制限なし、`network.sync_freq=1`、接続猶予32,000tick、同じ元の測定セーブに揃えると、
**現行の製品ビルドでも途中参加の1tick後にSTATION乱数不一致を再現**した。
したがってCPU速度差だけが原因ではなかった。
読み取り専用の診断ログで駅評価の入力を比較すると、読み込み時の
`AfterLoadPassengerJourneys()`が待機旅客を再分類し、ホストと異なる次ホップ別キュー数にしていた。
例えば駅3186の旅客は、待機9人・評価48・処理前の乱数seedが同じでも、
ホストは3キュー、途中参加側は1キューだった。直後の駅3245では乱数seedが分岐していた。
駅評価の削減量はキュー数を使い、`StationCargoList::Truncate()`はpacket単位に乱数を引くため、
この再分類が乱数列を変える。最初の参加者はホストと同じ初期キューを読むため同期していた。

修正は読み込み時の待機キュー再分類を取り除き、保存されたキーとpacket順序を維持する。
通常の経路更新・乗車処理が全peerで同じ時点に検証と再分類を行う。
実乗車の適格性検証は維持し、乱数比較や同期検出を無効にする変更はしていない。
追加のpacket保存・読み込み回帰は修正前に6箇所で失敗し、修正後は23 assertionsすべて成功した。

同じ元セーブ、製品構成（`OPTION_PASSENGER_BENCHMARK=OFF`）、CPU制限なしで再試験すると、
修正後はstate ticks `3176545487→3176547993`の2,506tickを進め、途中参加後90秒の同期を維持した。
初期／途中参加のクライアントは終了確認時も接続し、同期ACKをそれぞれ34回／27回送信した。
同期不一致・タイムアウトは0件だった。
ユーザー提供の公開版セーブでも、OD割当とJourney Routingを有効にして90秒進め、
**Journey旅客104人／37packet**が存在する状態で途中参加させた。
修正後の製品ビルドでstate ticks `3179509022→3179514778`の5,756tickを進め、
途中参加後さらに90秒、初期／途中参加のクライアントはそれぞれ82回／42回の同期ACKを送信した。
両方のCPU affinity maskは`4294967295`（制限なし）で、終了確認時にも接続していた。
同期不一致・タイムアウトは0件だった。
修正後の全回帰は**CTest 476件すべて成功**（51.86秒）。
全4交通手段の施設変更ケースも、保存状態をロード時に維持し、不適格な乗車は拒否して
通常の経路更新でFallbackへ移ることを検査するよう更新した。
ここまでの性能表は修正前コミットの測定値であり、このロード修正後の性能再測定値ではない。

[修正前の元セーブ再現](measurements/passenger-journey-periodic-2026-10-12/mp-unrestricted-original-before-result.json)、
[不一致の抜粋](measurements/passenger-journey-periodic-2026-10-12/mp-unrestricted-original-before-desync-excerpt.txt)、
[駅評価入力の差](measurements/passenger-journey-periodic-2026-10-12/mp-station-rating-differences.json)、
[修正後の元セーブ](measurements/passenger-journey-periodic-2026-10-12/mp-unrestricted-original-after-result.json)、
[修正後の提供セーブ](measurements/passenger-journey-periodic-2026-10-12/mp-unrestricted-supplied-after-result.json)、
[検証バイナリ・セーブ・ソースの識別](measurements/passenger-journey-periodic-2026-10-12/mp-sync-fix-metadata.json)、
[全回帰ログ](measurements/passenger-journey-periodic-2026-10-12/mp-ctest-final.log)。
再実行ハーネスは`--cpu-mode unrestricted --timeout-ticks 32000 --net-debug 7 --duration 90`で
速度条件を揃え、接続数だけでなく終了時の接続維持・同期ACK・desync reportを確認する。
実行ファイルはログ取得のためPEのconsole指定だけを変更した複製で、実行コードは製品版と同じ。
元のユーザーセーブとNewGRFは変更していない。

別途、NewGRFなしの回帰マップへ所有会社、実道路・depot・bus stop、実バス、
明示ダイヤ、50人のOD旅客を準備して私設loopback serverを起動した。
初期構築前と公開後に2クライアントを参加させ、両方を実際にCPU mask 1へ制限した。
ホストは制限せず、`network.sync_freq=1`で毎frameの同期を検査した。
state ticks `13639680→13642823`の3,143tickを進め、2クライアントは終了確認時点でも接続しており、
同期不一致を検出しなかった。1系統の時刻付き公開、実乗車31人、駅残留19人を記録した。
この区間の配送は0人であり、配送成功とは報告しない。
保存進捗・位相・予約状態の連続/復元一致は単体回帰と、このホスト連続実行/途中参加比較で検査した。
WANや全車種の長期ネットワーク試験まで保証する結果ではない。

[同期結果・CPU affinity](measurements/passenger-journey-periodic-2026-10-12/mp-controlled-result.json)、
[制御ケースの実ログ](measurements/passenger-journey-periodic-2026-10-12/mp-controlled-console.txt)、
[再実行ハーネス](measurements/passenger-journey-periodic-2026-10-12/Exercise-Multiplayer.py)、
[検証メタデータ](measurements/passenger-journey-periodic-2026-10-12/verification-metadata.json)を保存した。
`--fixture`は測定ビルドだけの準備コマンドを使い、クライアント接続前の隔離マップを変更する。
主性能比較の実セーブには実行していない。製品構成ではそのコマンドを含まない。
