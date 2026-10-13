# 周期ダイヤの次発検索索引

ベースは `jgrpp-ya` の `a58d472382f85c63fe9114888d71d7d189c45572`、
作業ブランチは `perf/passenger-departure-index`。
通常車庫のWait未設定対応、公開スケジューラー、JPSDの保存形式は維持する。

## 発車時刻とライブ補正

公開ServiceとCargoTypeごとに、正の容量を持つ既知位相を索引化する。
容量の生値は元の公開車両レコードに残り、索引は現在のPlanと同じ非ゼロ条件を使う。
OrderListIDをキーに含め、incarnation、generation、periodを照合する。
各実車両のIDと絶対StateTicksのbaseを保持し、同一位相もまとめない。
不明位相で正の容量がある場合のUnknownPhase観測を保持する。

offsetを駅Occurrenceの発車オフセット、rをready、pをperiodとすると、
元のNextDepartureは初回 `f = base + offset` 以降の `f + k*p` の最小値を返す。
全車両を次の2集合に分けてそれぞれ最小値を求める。

* `base <= r-offset`：初回発車済み。正規化したbase剰余の巡回successorを求める。
  剰余順segment treeの最小baseにより、まだ初回発車前の車両を探索から除く。
* `base > r-offset`：初回発車前。base順のupper_boundから最小の初回発車を求める。

`r-offset`がINT64_MINを下回る場合は全車両が後者に入る。
剰余は負tickも非負へ正規化し、引き算と加算の表現範囲を個別に確認する。
初回発車・次の巡回発車のオーバーフローは元の実装と同じ無効候補になる。
全車両が開始済みでライブ除外がなければ、剰余順のlower_boundで選択できる。
構築はO(N log N)、ライブ補正なしの位相照会はO(log N)。
系統・貨物別キャッシュ行の取得には別途mapの検索がある。

駅のloading_vehiclesから、元の全車両走査と同じ安全条件を満たす公開車両を確認する。
停止・事故・Scheduled Dispatch・Loading Advance・別の指令Occurrence・暗黙指令・
別駅・異なるtimetable index・異なるOrderList incarnationには補正を適用しない。
残りの公開dwellからbaseを計算できた車両は、静的候補から**除外して置換**する。
単にライブ便を追加すると、その車両の古い予定便が未来readyの照会に残ってしまう。
駅の一覧がK台、対象Occurrenceのライブ補正がL台なら、局所確認はO(K log N)、
ライブIDのソートはO(L log L)、除外を含む索引照会はO((L+1) log N)となる。
駅が削除済みで局所観測できない場合と、Scheduled Dispatchは旧走査を使う。

## Work、公開世代と保存

通常系統の全車両走査は容量・位相に関係なくN単位を課金する。
残予算がNより**厳密に大きい**ときだけ索引を利用し、同じN単位を一括課金する。
残予算がN以下なら元の車両順・spend・観測をそのまま実行する。
Nと等しい場合にも元のpost-scan Work停止が発生するため、等号は高速経路に含めない。
Dispatchは車両とスロットの各spendを維持し、すべて旧走査する。
順序に依存する部分走査を集計Workから推測しない。
検索の論理メモリ、候補・ラベル制限、採用人数の予算は変更しない。

索引は公開済みServiceだけから遅延構築する。buildingには索引を付けない。
Publicationで実際に置換・削除するIDの行だけを解放する。
全周分のbase差だけで同じ公開世代を維持した場合も、元の初回下限と索引を維持する。
TimetableChangedと構築中のSave/Loadは旧公開世代を使う。
tick再基準化時には剰余と初回下限が変わるため全索引を破棄する。
Load後は空の派生キャッシュから再構築し、ローカルの有効・無効設定だけを維持する。
Service、Vehicle IDの再利用は公開incarnation/generationと既存のライブ検証で扱う。

派生索引の物理領域は8 MiB。構築前に `512 + 128*N` bytesの保守的領域を確認し、
収まらない場合は旧走査する。保持量にはvectorのcapacity、segment tree、map nodeを含む。
この領域とキャッシュ設定はセーブ・論理Work・検索メモリadmissionに含めない。
キャッシュ有無でPlanの結果や延期を変更しない。

## 診断と再現

`passenger_journey_stats departures_on|departures_off`でローカルの索引を切り替える。
旧 `next_departures` は旧走査相当の論理評価数を表示する。
新しい `Periodic departures` 行は、論理車両候補数、論理発車評価数、物理的な位相計算数、
hit/fallback、構築・再構築の回数と時間、保持・ピーク領域、前向き検索時間、検索Workを表示する。
索引の更新は公開時の解放と新世代の遅延構築であり、そのコストは公開・前向き検索scopeにも含まれる。
壁時計と診断カウンターはルーティング入力にならない。

[測定スクリプト](measurements/passenger-departure-index-2026-10-13/Measure-Departures.ps1)は、
同じ入力・バイナリ・設定で索引だけを切り替え、指定tick数後に診断とセーブを採取する。
[集計](measurements/passenger-departure-index-2026-10-13/Summarize-Departures.py)は
Work、論理評価数、延期・理由、輸送、最終セーブハッシュを照合する。
Save/NewGRF/exeはコミットしない。

CPUサンプルは速度測定と分離し、対象プロセスの最もCPUを使ったスレッドだけを採取した。
MSVC /MAPと命令位置を照合したexclusiveなサンプルで、スタックを展開していない。
inline処理は呼び出し元へ帰属する。旧走査の1536tick実行のうち25秒を採取し、
実行ファイル内3085点・実行ファイル外262点、取得失敗0。
Plan本体316点、ラベル挿入lambda 377点、LoadingBase 42点、NextDeparture 12点だった。
NextDeparture回数の減少だけからJourney全体の同率の高速化を推測しない。

## 回帰

索引単体は1・2・40・150台、同一位相、異なる初回base、貨物別容量、不明位相、
ライブIDの除外、INT64_MIN/MAX、最大周期、負ticksを旧NextDepartureの全走査と比較する。
密な3500 ready照会と、初回下限を無視すると誤った車両を選ぶ明示例を含む。
Planは通常・繰返し停車・NoLoad/NoUnload・強制降車・車庫Waitなし・Waypointの各ネットワークで、
Work上限0～699を各車両数で比較する。Dispatchのスロット途中停止もWork上限0～99で比較する。
ride、全route、費用、到着、transfer、Service/Order Index、Work、延期、理由、観測、
labels、candidates、論理メモリをcold/warm/毎回索引破棄/Load済みpeerで照合する。
遅延した実乗降と未来ready、ライブ補正の各安全条件、公開待機、再公開、ID再利用、
負tick再基準化と索引設定のLoad保持を含む。
テスト用の乗降車両一覧も実運転と同じ登録・移動・削除に合わせた。

旅客全体は328 cases / 1,302,856 assertionsが成功した。
計測オプションOFFの製品構成では全541件を検証した。
初回は538件が成功し、3つの製品プロセス試験が起動データ不足で失敗した。
`-Q`がTAR走査を省くためOpenGFXを展開し、exeのあるReleaseからbaseset/langを
参照できるようにして再実行したところ、残る3件も成功した。
この修正はビルド環境のデータ配置で、ソースやテストの判定条件を変更していない。

[旅客回帰](measurements/passenger-departure-index-2026-10-13/passenger-tests.log)、
[全CTest](measurements/passenger-departure-index-2026-10-13/ctest-product.log)、
[3件の再検証](measurements/passenger-departure-index-2026-10-13/ctest-product-retry.log)。

## 同じ実マップ・8192tickの比較

入力は前報告のverified-result.savと同じコピーで、SHA256は
`14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`。
同じ計測用exeで索引だけを切り替え、3176545487→3176553679の8192tickを逐次実行した。
MSVC 19.44 x64 Release /MT、assert ON、LTO OFF、null video/sound/music。
ビルド、回帰、CPUサンプリング、他のゲーム実行と重ねていない。
既存NewGRFの容量変更警告は両側にあり、隔離したコピーの計測用unpause処理を使った。
今回のベースは車庫Waitなし対応を含み、末尾のTimed系統は652、車両は3664だった。
前報告の506系統・2837車両・前向き6.17msとは負荷が異なり、その数字へ直接比率を取らない。

| 指標 | 旧走査 | 索引有効 |
|---|---:|---:|
| 論理車両候補 | 376,416,359 | 376,416,359 |
| 論理発車評価（旧next_departures） | 355,283,103 | 355,283,103 |
| 物理的な位相計算 | 355,283,103 | 84,739,772 |
| 検索Work | 5,469,210,334 | 5,469,210,334 |
| 索引hit / fallback | 0 / 58,368,819 | 58,362,003 / 6,816 |
| 索引構築・再構築 | 0 | 8,300回 / 21.453ms合計 |
| 索引保持 / ピーク | 0 | 419,660 / 420,800 bytes |
| Timed前向き検索平均 | 16.467ms/tick | 16.441ms/tick |
| Timed照会全体平均 | 21.707ms/tick | 21.648ms/tick |
| Journey平均 / p95 / p99 | 26.327 / 45 / 68ms | 26.264 / 45 / 70ms |
| simulation平均 / p95 / p99 | 52.245 / 75 / 97ms | 51.642 / 73 / 96ms |
| プロセスpeak working set | 2091.00MB | 2094.54MB |

p95/p99は1msヒストグラムの上端。物理計算は76.1%減ったが、前向き時間の差は0.16%、
Journeyは0.24%、simulationは1.16%で、1組の比較から明確なCPU改善を主張できない。
Journeyのp99は2ms増えた。最終セーブ、Work、候補数、延期・理由、輸送件数は一致した。
最終セーブSHA256は両側とも
`1bae3398586e1abfac6d305d9af825a2d26e76cbc69fc21369d2549f5a61c590`。
Timed実乗車49,655人・乗換20,896人・最終配送15,311人も一致した。

[集計と同一性](measurements/passenger-departure-index-2026-10-13/summary.json)、
[旧走査ログ](measurements/passenger-departure-index-2026-10-13/scanned-console.txt)、
[索引ログ](measurements/passenger-departure-index-2026-10-13/indexed-console.txt)。

## 改善が小さい原因と限界

索引構築・再構築は0.0026ms/tickで、維持コストの主因ではない。
索引解放を含む公開scope全体は旧120.693ms、索引有効130.781ms合計。
解放だけを分離計測していないが、その上界でも0.016ms/tickである。
保持ピークも約0.40MiBだった。

索引有効の別実行でも25秒のCPUサンプルを採取した。
実行ファイル内3071点・外278点、取得失敗0。
Plan本体は316→260点となる一方、DepartureIndex取得28点、Next18点、Find8点、Matches1点を観測した。
ラベル挿入は377→339点、費用下界lambdaは143→152点、逆CompilePolicyは97→128点で、
元の順序・ラベル優越・費用下界・Work境界を保つ検索処理が大きく残る。
共通のmapやlower_bound内部、inline処理は上記の関数だけに帰属しない。
サンプルの時間位置も完全には同じであり、これらを厳密なCPU時間の差分とは扱わない。

実マップの論理候補数/発車照会数は平均約6.45台で、150台の純粋な位相照会ほど
大きなNではない。追加のキャッシュ取得・successor探索・局所ライブ確認があり、
車両位相計算回数の減少は、ラベル処理を含む前向き検索全体の同率のCPU減にはならなかった。
ラベル/下界処理が残ることと、新しい照会処理への置換が観測されたことは根拠があるが、
サンプリングだけから全停滞コストを一意に帰属できるとは主張しない。
論理Workを減らすことによる採用人数の変更で時間を短縮する実装はしていない。

[旧CPUサンプル](measurements/passenger-departure-index-2026-10-13/cpu-scanned.json)、
[索引CPUサンプル](measurements/passenger-departure-index-2026-10-13/cpu-indexed.json)。

## Save/Loadと途中参加

同じ実マップの最終セーブから私設127.0.0.1で初期・途中参加の2クライアントを接続した。
サーバーは索引有効、clientsは索引・ポリシーキャッシュ無効で、各clientを1 coreに固定した。
sync_freq=1、3176553679→3176554634の955tick進行を確認し、同期不一致の検出は0件。
末尾も両クライアントは接続中だった。
既存GRFの警告を解除する計測用console複製を使用し、速度比較とは分離して実行した。
製品オプションOFFの保存・GUI・診断不変性は上記の製品プロセス回帰で検証した。
有限のloopback試験であり、WANや長期運用全体の保証ではない。

[途中参加結果](measurements/passenger-departure-index-2026-10-13/mp-result.json)、
[サーバー診断](measurements/passenger-departure-index-2026-10-13/mp-server-console.txt)、
[バイナリ・ソース識別](measurements/passenger-departure-index-2026-10-13/verification-metadata.json)。
