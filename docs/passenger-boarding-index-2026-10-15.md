# 旅客の乗車判定索引

対象は `codex/passenger-precomputed-completion`。`f880eac3bc` からの修正に、
共通区間の乗車束縛修正 `378a32e3ba` を取り込んだ。
事前計算モードの乗車候補を、Packetの配列ではなく集団のiteratorで取り出す。

## 候補と検証

- Journeyは保存された系統ID・束縛された実車ID・乗車駅・乗車orderで直接検索する。
  未束縛の同等系統も扱う。降車位置、世代、時刻、到着系統が同じ集団で
  乗車検証を共有する。別の実車やorder向けのPacketは列挙しない。
- 共通区間のキーも、保存された系統とorderの組から作る。現在と保持中の旧表の
  両方を使って元の集団キューを直接参照し、同じ集団が複数のキーに当たっても
  一度だけ処理する。同じ系統の別の適合する乗車orderへの束縛も扱う。
- ODのないLegacyはCargoDistのnext-hop bucketで直接検索する。
- ODのあるLegacy/Fallbackは目的地・保存済みhop・到着系統とincarnationで集団化する。
  乗換前の集団は時刻キューへ置き、乗換可能になってから有効化する。
  到着した系統への即時再乗車は集団単位で除外する。
- 列車の実際の降車可能prefixに対して、目的地への直行、保存済みhop、
  公開済み経路表の残費用が減る降車駅を引く。経路表の逆引きは表の行公開時に
  構築し、置換した旧行は旧世代の逆引きからも外す。構築中は残っている旧行も使う。
- 表は推定経路の候補を出す。乗車は実際のorder・降車可否・強制乗換・refit・
  分岐の扱いを含む運行prefixの検証を通す。表だけでは乗車を許可しない。

事前計算モードのFallbackでは乗車のたびにCargoDistの重みやLinkGraphを探索せず、
保存済みhopを使う。hop更新は従来の旅客ルーティングイベントで行う。
表に利用できる行がなくても、実際に直行できる目的地と保存済みhopへの乗車は可能。
表もhopもない場合の乗車時OD探索は行わない。事前計算を無効にした方式は維持する。

候補のマージ用heapが保持するのは集団のcursorだけで、Packet数分のvectorを作らない。
同じloading処理では、実車・CargoType・OD・経路実行情報・乗換条件が等しい検証を
再利用する。hashだけの一致判定にはせず、全条件を比較する。
高コスト検証はFIFOの先頭へ来た集団だけに行い、不適格ならその範囲全体を飛ばす。
満員になれば後方の集団は検証しない。事前計算モードでは、ローカルな検証・OD・
車両prefixキャッシュが128種類を超えただけで乗車を拒否する制限も設けない。
tick、系統の安全性、表の公開状態、Packet条件の変更で再検証し、
refit時の `Invalidate(vehicle)` はその実車の検証だけを破棄する。
キャッシュはloading処理の寿命を越えず、保存状態や処理枠には使わない。

## 順序と公平性

基点のJourney候補は既に `(主bucket, queue sequence, PacketID)` 順であり、
PacketID順そのものではなかった。ただしJourneyを先に取り出し、その後でLegacy全体を
調べる優先順位が残っていた。

変更後は候補となる全方式を同じheapで扱い、主bucket間を持ち回る。
前回実際に乗車または予約したbucketの次から開始し、末尾から先頭へ循環する。
各bucket内は保存される主待機リストのFIFOを維持する。
同じbucket内で新しい小さい再利用PacketIDが先着客を追い越すことはない。
これにより、ある方式や低いbucketが毎回無条件に先頭になることを避ける。

持ち回り位置を `passenger_periodic` v10へ保存する。v9以前は先頭から開始する。
DB blobはv8のまま。queue sequenceの絶対値は保存せず、主リストから同じ相対順序を
復元する。索引の破棄・再構築、保存・読込、MP参加で乗車順序を変えない。
同じ条件のPacketが結合される場合は、既存の結合先集団の待機位置を引き継ぐ。

## 更新範囲と負荷

分割・結合・予約返却・削除は、既存のCargoMovementとPacket変更通知を使い、
対象Packetの索引だけを更新する。人数・支払い・merge条件だけの変更では、
乗車集団を作り直さない。期限だけの変更はイベントキューだけを更新する。
系統・表の世代変更でも全待機Packetを即時走査しない。

候補Packetの検証数は、対象となる集団・実際に乗車するPacketに依存する。
無関係なPacketが同じ駅に増えても、それらを乗車候補として調べない。
表の逆引き検索は駅ネットワーク・待機OD集団の種類数・列車のprefixに依存し、実際に乗車可能な集団が
増えた場合の候補処理と物理的なCargoPacket移動の費用は残る。
追加メモリは待機索引が概ねO(Packet数)、逆引きが最大O(表の駅数×目的地数)。
逆引きの領域も事前計算表のメモリ上限へ計上する。

## 検証内容

対象外のLegacy/Fallback、別実車・別order、乗換時刻と到着系統の除外、
降車禁止の変更、検証メモの系統失効、128を超えるFallback OD、人数のみの変更、
小さい再利用PacketID、巡回iteratorの境界削除、予約分割・返却、索引再構築、
v8/v9/v10の保存テーブル、公開途中・置換途中の経路表逆引きを回帰テストに含める。

性能測定は `openttd_test.exe "[precomputed][boarding][fallback][cpu]"`。
待機する対象外Packet数を128、1,024、8,192、32,768へ増やし、
同じ乗車対象への1,024回の照会について、Packet・集団の検査数と高コスト検証の
再実行数をCPU時間とともに記録する。

製品設定はMSVC x64 Release、assert ON、LTO OFF、Passenger Benchmark OFF。
統合後の全CTest **661/661成功**、事前計算の通常回帰は**55ケース・72,628アサーション成功**。
乗車のCPU測定は**4ケース・198,652アサーション成功**。

| 対象外Packet数 | Fallback照会時間 ns | 1,024回のPacket検査 | 集団検査 | 高コスト再検証 |
| ---: | ---: | ---: | ---: | ---: |
| 128 | 632 | 1,024 | 1,024 | 0 |
| 1,024 | 591 | 1,024 | 1,024 | 0 |
| 8,192 | 606 | 1,024 | 1,024 | 0 |
| 32,768 | 605 | 1,024 | 1,024 | 0 |

初回に1集団を検証した後の同じloading処理での値。各照会の対象は1 Packet・1集団。
時間は単一ホスト上の合成測定であり、ホストの他の作業は排除していない。
実マップ全体のsimulation tickの時間を示す値ではない。
[全CTestログ](measurements/passenger-boarding-index-2026-10-15/checks/ctest-integrated.log)、
[事前計算の回帰](measurements/passenger-boarding-index-2026-10-15/checks/precomputed-integrated.log)、
[性能の生ログ](measurements/passenger-boarding-index-2026-10-15/checks/boarding-scaling-integrated.log)を参照。

### 実マップ

前回の保存（SHA-256 `0e2ae908db8d3bf75c0ecae4f5d680a187d920fb9504e129f88dee4185ace22d`）
から、共通区間束縛の統合前に**1,024 simulation tick**を実行し、v10保存を生成した。
この保存を統合後のMP検証の入力にも使う。
終了時は488,096 Packet、Journeyは250,709 Packet、既知ODの人数は806,628。
支払い検査は正常で、運行中のエラー停止の再開は0回。

表の準備は約135秒、プロセス全体は約314秒、最大working setは約6.50 GB。
231 tick後にLinkGraph計算待ちの停止を挟んでいるため、プロセス全体の時間を
旅客の通常tickの経路処理時間とはみなさない。
開始時のエラー停止解除を含む既存の測定ドライバーを使った。
SP側のWindows標準出力・標準エラーはこの実行では取得できていないので、
警告件数0という判定には使わない。入力の既知NewGRF警告の解消は対象外。
[実マップ結果](measurements/passenger-boarding-index-2026-10-15/real-map-process.json)、
[Packetと支払いの検査](measurements/passenger-boarding-index-2026-10-15/real-map-integrity.json)、
[実行tick記録](measurements/passenger-boarding-index-2026-10-15/real-map-ticks.json)、
[コンソール記録](measurements/passenger-boarding-index-2026-10-15/real-map-console.txt.gz)を保存した。

### 実MPと製品への復帰

そのv10保存から統合版でローカルMPへ参加し、参加直後と**1,030 tick**の運行後に、
Packet・支払い・DB・主待機リストの順序・予約・loading状態・駅の保存cursorを
含む全比較項目が一致した。同期エラー・切断はなかった。
参加時の表再生では13,321回に分割し、最大loop時間は13.907 ms。
その間、StateTicksとframeは進んでいない。

ゲームのタイムアウトは既定値のまま。統合前の最初の試行は、再生が進んでいる間に
補助ツールの180秒の観測上限へ達して中断した。ネットワークエラーではない。
補助ツールに `--observe-timeout 600` を追加し、統合版では約225秒で参加を完了した。
これはゲームの接続タイムアウトを変更しない。
MPの標準エラーには既知のJP+ NewGRF警告が両側とも28,521行あり、その記録も保存した。

測定用TUとフックを参照しない製品設定へ戻して再ビルドし、通常の事前計算回帰
55ケース・72,628アサーションも再度成功した。
製品binaryに測定用の保存ファイル名・環境変数名・終了スクリプトがなく、
projectにもコピーした測定ソースの参照がないことを確認した。
[全結果・コード差分hash・製品確認](measurements/passenger-boarding-index-2026-10-15/integration-validation.json)、
[MPの完全結果](measurements/passenger-boarding-index-2026-10-15/mp-integrated-result.json)、
[初回の観測上限による中断](measurements/passenger-boarding-index-2026-10-15/mp-observation-180s.json)、
[製品復帰後のビルド](measurements/passenger-boarding-index-2026-10-15/checks/build-integrated-restored.log)を参照。
統合前の測定は[別の結果JSON](measurements/passenger-boarding-index-2026-10-15/summary.json)に残し、
それぞれの基点へ適用できるコード差分もgzipで保存した。
