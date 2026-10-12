# 周期Journeyの候補選別・位相・利用率（2026-10-13）

ベースは `7d914aa6f28293abb406becee2cec5fbb11e1d17`、ブランチは
`fix/passenger-periodic-timed-coverage`。直通を先に8件登録する処理を廃し、
実際の発車位相・待ち時間を含む探索で直通と乗換を比較する。
全国ServiceGraphや物理Visit列は通常処理に追加していない。

## 探索と上限

`CompilePolicy` が保存するのは候補経路ではなく、目的地から逆向きに調べた
時間下界である。既存の駅・周期stop索引から隣接stopを読む。乗降制限、待ち時間、
途中停車時間を緩和した距離なので、候補を名目時間で確定することはない。
Originが確定したところで逆探索を止める。未確定駅には最後の確定距離を下界として
使い、接続なしと扱わない。Workで途中終了した場合も、確定済みの下界だけを使う。

前向き探索は各車両の保存位相と実Dispatchスロットから次発を求め、実際の到着時刻と
非負の一般化費用をキューに入れる。完了候補はキューから取り出した時に評価する。
最良の完了経路を保持し、候補の診断上の集計枠は8件までとする。枠に達しても、最良経路を改善・同費用で優先できる接頭部を打ち切らない。追加候補は枠を増やさず最良順位だけを更新し、全探索ラベルはLabel/Work/memory上限で拘束する。
同一系統でそのまま乗り続ける結果を親で走査済みなら、より早い共有車両への乗換だけを
再走査する。NoLoad、NoUnload、強制降車、繰返しstop、異周期の接続を保持する。

ActiveのWork 65,536、Labels 8,192、scratch 8 MiB、最大8候補、DB 64 MiB、
policy cache 16 MiBは変更していない。Labels・memoryで落とした候補の下界が最良費用以下なら
`deferred` を返す。Work打ち切りも経路なしに変換しない。任意のpolicy cacheは
保存しないが、ヒットでもコールドの論理Workを再課金する。

## 位相

停車中で設定待ち時間を超えていた車両の残り待ちを0とする。
旧SnapshotのAssumeTimetableと同じで、すでに停車している実車の現在位相を使う。公開後に待ち時間を超えた実車についても、新規検索時に駅・指令・incarnationが一致する実停車を定数時間で照合する。残り待ちと次周回を計算し、公開位相やcacheを変更しない。
従来の `now - elapsed` では、設定発車を過去に置き、次周回まで待つ候補になっていた。
waypoint・通過指令も、一周中の実指令位置として捕捉する。
未到着で設定走行時間を超えた車両はUnknownとし、実駅到着で再捕捉する。
位相既知でも実到着が保存位相からずれた場合は、その系統を局所再構築する。
時刻表だけのDirty世代へ、異なるライブ時刻表の位相を混ぜない。

tick末尾で施設検査Workが不足した系統を、Unknownのまま恒久公開しない。
公開前にDirtyへ戻し、次tickの予算で再試行する。4,096 Workの個別施設検査上限は維持する。
共有Dispatchのスロット所有競合、Unbunching、施設不適合、実指令の不一致は保守的に扱う。
共有車両の等間隔配置や架空の発車を作らない。

## 診断の単位

`passenger_journey_stats total` で以下を出力する。

* **live census**: その瞬間のOrderList数。旅客コンパートメントの実容量がある系統だけを
  passengerの分母にする。貨物専用系統は除く。現在の周期コンパイル成功・失敗と最初の失敗理由、
  公開なし、Dirty、再構築待ち、旅客車両の既知・未知位相を数える。
* **compilation observations**: 公開処理で行ったコンパイル回数。再構築を含み、同じ系統が
  複数回入る。Work・memoryによる公開拒否/再試行も累計処理観測であり、一意系統数ではない。
* **OD calls**: DBの `Plan` 呼出し回数。`outcomes` は排他的で合計が検索数になる。
  `overlapping_observations` は理由ごとに1検索につき最大1回。FoundとUnknownPhase、
  逆探索Work不足とFoundなどが重複する。UnknownPhase観測は遭遇した未知候補を含む。
  既知位相では接続せず、位相を外した乗降制約付き探索で接続を確認した場合は
  UnknownPhase outcomeとして区別する。
* **execution observations**: 失効packet観測と、その時のpacket人数。同じ旅客の再適用・再失効を
  含む。boarding failureは同一系統への検証失敗回数で、満員・実乗車人数とは別。
* **実乗車・乗換・配送**: 既存のCargo移動処理で実際に動いた人数。予約は乗車と数えない。

停止イベント内の同一OD共有はDB検索を増やさない。DBへの入場前に既存のtick/event quotaで
延期されたpacketは、ActiveのQueryWork/QueryLimit診断に入る。これをDBのWork outcomeや
検索人数と合算しない。ライブ位相理由は現在状態の説明であり、過去の捕捉時の理由を捏造しない。
自動時刻表編集のDirtyでは、ライブ完全周期と公開済みの古い周期が異なることがある。

## 同一状態での比較

`passenger_periodic_compare <cargo> <origin> <destination>` または
`passenger_periodic_compare sample <1..256>` を明示実行する。
同じ車両・指令・セーブ由来の公開DB・StateTicksを読み、DBの独立コピーで周期Plannerを
呼び出す。旧方式は診断用のscoped Snapshotだけを構築する。シミュレーションを進めず、
packet、RNG、公開進捗、実DB cache/検索診断を変更しない。

各ODの旧/新の発着、全乗換leg、系統公開状態、指令位置、経過・遅延、Dispatch、位相理由、
候補数・Labels・Work・memory・選択できなかった理由を出す。サンプルは駅pool順に既存の
待機packetから一意ODを取るため、全ODの無作為標本や旅客数比率ではない。
通常ゲーム中に二重探索を常時走らせる機能は追加していない。

## 実マップ測定

前回と同じ入力SHA-256
`14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390` を隔離コピーした。
同じ資産、MSVC 19.44 x64 Release /MT、assert有効、LTO無効。
null:until_exit/null/null、OD/Journey ON、PAX Asymmetric、recalc 8/32。
StateTicks `3176545487 → 3176547014`、完了tick標本1,526。
測定用 `OPTION_PASSENGER_BENCHMARK=ON` のみ既存NewGRF Error pauseを解除する。
元セーブを変更していない。性能測定とビルド・テスト・比較診断は重ねない。

測定結果と同一状態比較の集計を次の表と
[生ログ・条件](measurements/passenger-periodic-coverage-2026-10-13/summary.json)に記録した。

| 同じ1,526tick標本 | 旧Snapshot（今回再測定） | 修正前周期（前回記録） | 最終修正版 |
|---|---:|---:|---:|
| Timed適用人数（再適用含む） | 30,781 | 8,061 | **23,552** |
| 適用packets | 12,423 | 4,003 | 14,298 |
| 時刻付き実乗車人数 | 11,226 | 2,693 | **9,056** |
| 時刻付き実乗換人数 | 21 | 0 | **0** |
| 時刻付き最終配送人数 | 2,371 | 645 | **1,355** |
| 全配送人数（Timed＋Untimed） | 32,352 | 32,002 | 32,421 |
| 終端待機人数 | 211,083 | 212,015 | 210,634 |
| 平均simulation tick | 265.536 ms | 38.876 ms | **38.042 ms** |
| 通常処理のServiceGraph構築 | 23,409 | 0 | **0** |

修正前周期の列は[前回の同一入力・区間の記録](passenger-journey-periodic-2026-10-12.md)。
今回の旧Snapshotと最終版は直列の新規プロセスで再測定した。時間は単回観測であり、
前回の289msを今回の旧方式の値として流用していない。最終版の適用・乗車・配送は
修正前周期より約2.92倍・3.36倍・2.10倍。旧Snapshotの人数を全面的に回復した結果ではない。
実乗換0人は未解消。全配送の69人増・待機449人減も、長期的な改善保証とはしない。

同じセーブから別々に進めた人数比較は、途中から旅客状態が分岐する。アルゴリズムの
原因比較は下記の同一状態診断と回帰テストで行い、別ランの人数差だけを因果の証拠にしない。
途中の試作 `coverage` / `coverage-final` / `coverage-live-final` / `accepted` も残すが、
採用版の結果は **`publication-final`**。最後に大共有系統の公開再試行を有界にしたことで、
終端の公開が343/837件から837/837件へ回復した。これは先行版との観測で、
両ランのODや旅客状態がすべて同一という意味ではない。

## 残る差の原因

最終の旅客系統は771件で、貨物専用66件を分母から除いた。OpenTTDの
`OrderList::IsCompleteTimetable()` を満たす宣言ダイヤは673件（87.3%）。
正の周期をコンパイルできたものは512件（66.4%）。失敗259件の最初の理由は
**Control 158 / Travel flag 93 / Wait flag 1 / ZeroPeriod 7**。
Refit、Overflow、TooFewStopsは0。完全な時刻フラグと、分岐やdepotを含まない
固定一周を証明できることは別であり、未設定を主因と決めつけられない。

終端では全771旅客系統を公開済み、公開周期509件、位相既知の系統503件。
旅客系統のTiming Dirtyは520件だが安全な旧世代は公開されたまま。
未公開は0。実旅客車両の既知位相は2,321、未知は1,390。
未知の現在理由はNoTimetable 1,032、VehicleState 164、OrderProgress 3、
ElapsedTravel 56、AwaitCapture 135。施設、共有Dispatch競合、Dispatch未対応、
Unbunching、個別4,096 Work上限は、この終端censusでは0。
これらをこのマップの主因とはしない。位相捕捉時のWork停止は累計91車両観測で、
現在Work未知の車両数とは別。公開Work拒否/部分捕捉は6タスク観測、memory拒否は0。

DBの24,416検索は排他的に以下へ分かれた。

| outcome（OD呼出し回数） | 件数 |
|---|---:|
| 有効なTimed経路 | 10,042 |
| Originが公開周期駅索引にない | 1,239 |
| Destinationが公開周期駅索引にない | 6,288 |
| 既知位相の緩和接続成分で到達しない | 2,673 |
| 位相だけを外した乗降制約付き探索では接続する | 33 |
| 探索Workによる延期 | 4,141 |
| NoCandidate / Invalid / NoAlighting / Evaluation | 0 |
| 候補枠 / Labels / memory / transfer ceilingによる拒否 | 0 |

候補枠の飽和を11検索で観測したが、改善可能な接頭部を打ち切らず採用できた。
UnknownPhase観測13,526、Evaluation観測965、Work観測4,172は上表と重複する。
特に逆向き下界のWork打ち切り後でも、前向き探索で最良を証明できればFoundとなる。
Work延期は検索の約17.0%。停止イベントのQueryWork延期packet数は入場前のquotaと再試行を
含み、4,141 OD検索や旅客人数と同一ではない。失効・実乗車検証の回数は
[raw console](measurements/passenger-periodic-coverage-2026-10-13/publication-final-console.txt)とJSONに残した。

大きい共有系統の全車両捕捉が65,536 Workへ収まらない場合に、再試行し続ける実装は採用しない。
Trainは指令数から施設検査の上界を計算し、他モードは個別4,096の上界を使う。
一周分と検証済みの車両位相を公開し、残りはUnknown。未捕捉車両の位相や均等な間隔を補完しない。
150台・100停車の回帰で、部分的な既知位相による経路採用と、残りの全国発見の完了を確認した。

## 同一状態の旧方式との差分

最終セーブのStateTicks **3,176,547,014**で、同じ256一意ODを読み取り専用で比較した。
旧方式だけ成功21、周期だけ成功133、両方成功24、両方なし78。
比較前後のStateTicksと実DB検索診断は不変。旧方式はActiveと同じK=1と
65,536検索Work、periodicはK=8。Snapshotの取得/予測Workと個別の停止理由も別に記録した。

旧方式だけ成功した**全21 ODの旧経路にControlで周期コンパイル失敗した系統が含まれた**。
OriginMissing 13、DestinationMissing 8。公開自体はされていても周期が0なので
時刻付き駅索引に入らない。旧Snapshotは未対応制御に達する前の有効な接頭区間を使える。
周期側は全周回の証明を要求しており、この差をダイヤ未設定・Dispatch・施設・候補8件のせいにしない。

例としてOrigin 3 → Destination 4、出発3,176,547,014の旧方式は
Service 13 / boarding order 46 → alighting order 47を選べた。周期側はその系統の
Control失敗とperiod=0によりOriginMissingとなる。さらに乗換ODの各leg・位相・公開状態を
[21差分を含むJSON](measurements/passenger-periodic-coverage-2026-10-13/paired.json)と
[比較console](measurements/passenger-periodic-coverage-2026-10-13/paired-console.txt)へ保存した。
この256件は初期の駅pool順のサンプルであり、全実マップの成否率へ外挿しない。

残る差として、全周回の未対応制御、保存された公開時刻表と現在の自動測定値の差、
捕捉後の実進捗ずれ、初回/乗換のminimum transfer、tick/event quotaと探索Workがある。
実乗換0人の全原因をこのサンプルだけで断定しない。
通常運転へ旧Snapshotや全国物理Visitを戻して差を埋める変更は行っていない。

## 回帰・Save/Load・同期

最終の製品構成（測定option OFF）は **旅客280ケース / 966,413 assertions、周期61ケース / 12,874 assertions、CTest 493/493** 成功。以下のログへ保存した。

* [旅客回帰](measurements/passenger-periodic-coverage-2026-10-13/passenger-final.log)
* [全CTest](measurements/passenger-periodic-coverage-2026-10-13/ctest-final.log)
* [周期回帰](measurements/passenger-periodic-coverage-2026-10-13/periodic-final.log)

10直通対200tick乗換と時刻依存の逆転、普通/特急の共通接続で並ぶ費用、11乗換、
異周期・個別共有位相、Circular/Repeated Stop、NoLoad/NoUnload/Forced Transfer、
ゼロを含むWork/memory、候補飽和、Label/transfer上限、冷/温/evicted cache、
純粋な比較、保存されるDirty/公開進捗、負のtick再基準化、RNGを検証した。
旧/current save、無効時CargoDist、GUI/Shadow/診断のsave purity、AI/GS回帰も含む。
初回の4 AI/GS回帰は未生成の資産で失敗し、正規の `regression_files` ターゲットで用意して再確認した。
最終の全CTestは資産込みで実行し、493件すべて成功した。

NewGRFなしの回帰マップへ実道路・depot・bus stop・busと50人のOD旅客を追加し、
私設127.0.0.1のサーバーへ初期/公開後の2クライアントを参加させた。
両クライアントのCPU affinityを1 coreに制限し、sync_frequency=1で検査した。StateTicks 13,639,680 → 13,641,878の2,198ticksで、途中参加後も両clientが接続し同期不一致はなかった。
既存車両も含むネットワークで時刻付き実乗車62人・配送27人を記録した。
[同期結果](measurements/passenger-periodic-coverage-2026-10-13/mp-controlled-result.json)、[console](measurements/passenger-periodic-coverage-2026-10-13/mp-controlled-console.txt)、[ログhashと取り逃したframe数](measurements/passenger-periodic-coverage-2026-10-13/mp-controlled-log-metadata.json)を保存した。遅いclientが一部sync frameを
取り逃すログは保存し、「全frameを漏れなく検査した」とは主張しない。

実NewGRFマップの途中参加ではSTATION乱数不一致が残る。
前回の元方式でも同形の問題が報告されており、今回の旧方式でも、途中参加後1tickのSTATION seed不一致を再現した。
[周期版の差分](measurements/passenger-periodic-coverage-2026-10-13/mp-real-periodic-desync-excerpt.txt)と[旧方式の差分](measurements/passenger-periodic-coverage-2026-10-13/mp-real-snapshot-desync-excerpt.txt)にそれぞれ残した。
NewGRFの容量変更警告を原因と断定しない。この実セーブの途中参加成功を保証せず、
別途の同期問題として結果・抜粋を残す。WANや全車種の長期同期、旅客の一意旅行時間は未測定。
[再実行ハーネス](measurements/passenger-periodic-coverage-2026-10-13/Measure-Periodic.ps1)、
[同一状態の集計](measurements/passenger-periodic-coverage-2026-10-13/Analyze-Paired.py)、
[私設loopback検証](measurements/passenger-periodic-coverage-2026-10-13/Exercise-Multiplayer.py)、
[入力・実行ファイル・テストのhash/条件](measurements/passenger-periodic-coverage-2026-10-13/verification-metadata.json)
を保存した。ネットワーク検証コピーだけPE subsystemをconsoleへ変更し、標準入出力を取得した。
入力セーブ、GRF、実行ファイル、完全な巨大警告ログは `build-coverage/` にありGitへ追加していない。