# 旅客事前計算ルーティング：残課題への追加実装

追記：Phase 4 の後続実装・検証は、[共通区間の頻度集約](passenger-common-section-frequency-2026-10-15.md)を参照。
下表はこのレポート作成時点の状態を保持する。

追記：この文書のMP途中参加の失敗に対する後続修正・再検証は、[ロード再生の分割](passenger-precomputed-join-replay-2026-10-15.md)を参照。この文書の測定結果と当時の未完了判定は履歴として保持する。

開始点は `feat/passenger-precomputed-routing` の `a650090b625b30687cc0e676e157d5d15984cb0b`。
同ブランチが別作業ツリーにあるため、変更は `codex/passenger-precomputed-completion` で行う。
前回の測定値・未達要件は [前回レポート](passenger-precomputed-routing-2026-10-15.md) を参照。

**全Phaseの完成・統合可能とは判定しない。** 今回の中心はシングルプレイの初期構築と、更新時の行の維持である。

## Phaseごとの実装範囲

| Phase | 状態 | 今回の変更と残り |
|---|---|---|
| 1 初期構築・再構築 | 部分実装 | SPのsimulation開始前構築、進捗表示、目的地行ごとの旧表維持・置換、保存復元。MP共通の初期バリア、系統取り込み・グラフ準備・ロード再生の完全分割、影響目的地だけの再探索は残る |
| 2 Packetイベント | 未実装 | 前回の駅単位イベント処理を維持。イベント時の全Packet走査は残る |
| 3 乗車索引 | 部分実装 | 候補vectorの生成・コピーを除去し、索引を直接走査。Packet ID順を主キュー順へ変更。Fallback/Legacyの系統・降車駅索引は残る |
| 4 共通区間頻度 | 未実装 | 異なる停車パターン間の集約は行わない |
| 5 輸送品質 | 部分検証 | 旧PacketID乗車順による待機増加を対照実験で特定し、FIFOで改善。長時間の距離・所要時間・再訪等と25%の代替経路許容幅の検証は残る |
| 6 保存・同期 | 未完了 | 実セーブ往復は一致。完成表をロードした実serverへの途中参加は既定期限で失敗。残りの実MPケースは未検証 |

## 初期構築

通常製品の `GameLoop()` に、シングルプレイの初期準備を追加した。
公開サービスの初回取り込みを終えてから全目的地行を作り、準備が完了するまで `StateGameLoop()` を呼ばない。
`StateTicks`、車両の移動、旅客の発生・乗降を進める必要はない。設定有効化時にも同じ経路を使う。
途中の描画・入力ループへ戻り、ステータスバーに完成行数と全行数を表示する。コンソールの
`passenger_journey_stats` では初期準備のelapsed nsを通常simulationの統計と別に出す。

1回の外側ループは8 msを目安にし、内側では従来と同じ65,536操作のsliceを使う。
壁時計は描画へ戻る時点を選ぶだけで、行の結果・公開順・simulation開始条件には使わない。
**8 msは原子的処理を含む実時間上限ではない。** 大規模共有車両群の取り込み、グラフ準備・ソート等は依然として超過し得る。

MPではこのローカルな準備処理を適用せず、従来の決定的なフレーム内構築を継続する。
各peerが準備できたかによってフレームを省略する変更はしていない。
サーバー・クライアント共通の開始バリアと、ロード中も受信を継続する設計は残る。
ネットワークのtimeout設定は変更していない。

## 目的地行を維持する更新

完成した表を退避し、別の固定入力から新しいグラフを構築する。
新表の目的地行が未公開なら旧表を参照し、各行の公開時に対応する旧行を直ちに解放する。
表全体を複製せず、旧グラフの行から参照を続ける。構造が変わっても行の対応付けは圧縮添字ではなくStationIDで行う。
全行の置換が完了したら、旧グラフと旧入力も解放する。

構築中に届く構造変更は、固定入力の進捗を毎回巻き戻さず、次の世代へまとめる。
旧表・新表のどちらから選んだ乗車Intentにも現行サービスのincarnation/generation検査を適用する。
時刻だけの更新は両表の実行用bindingを更新し、推定コストの更新を待つ間も使用する。
乗車直前の指令・施設・乗降禁止・強制降車・容量・予約の検査は既存処理を使う。

保存形式はJPSD v7へ更新した。新表の固定入力・操作カーソルに加え、旧表の固定入力・revision・乗換コストを保存する。
ロードでは旧表を再現し、新表のカーソルを再生するときに同じStationIDの旧行を解放する。
旧v1〜6の読み込みを維持する。**ロード時再生自体は同期的であり、ここでの長時間占有は解消していない。**

論理メモリ枠は全旅客CargoTypeと両世代を合わせて16 GiBとした。
両世代の「全行が存在する」保守的なenvelopeを合算するためであり、実際に2枚の全行列を保持するためではない。
新行の公開コールバックで旧行を解放するので、同じ駅・状態数なら常駐する行数は概ね1世代分に留まる。
グラフの状態数や利用駅集合が大きく変わった場合には追加メモリを要する。
メモリ許可判定にローカルな空き容量やnative sizeofを使わず、物理メモリ不足はpeerだけ別経路へ切り替えず停止する。
32bit実行環境で大きな表を使用可能にしたとの主張はしない。

## 乗車候補の直接走査と公平性

候補を先にvectorへ取り出す処理をなくし、サービスの索引とFallbackの索引を直接たどる。
候補が1件見つかれば、その後ろの同系統の大量Packetをコピーする必要もない。
実際の移動処理が現在のPacketを削除・分割・再挿入することを考慮し、次のiteratorを確保する。
最後のPacketの削除で索引のbucket自体が消える場合も、そのiteratorを再参照しない。

優先順位は、対象サービスIntentの集団を先に処理し、各集団内では主キューのbucket順・bucket内FIFOとする。
同じ集団で、番号の小さい新規・再利用Packetが既存の待機客を追い越さない。
索引専用の順序番号は導出状態であり、保存済みlist順から再構築できる。
属性変更、人数変更、予約の部分分割で残ったPacketには同じ順序番号を使う。
予約返却で新規に主キューへ追加されるPacketは既存Append/TryMergeの位置に従う。
異なる集団間の厳密な公平性や、Fallback集団の広範な走査の解消は、この変更だけでは完成しない。

## 検証・測定

`utils/passenger_precomputed_real_map.py run` に `--rebuild-precomputed` を追加し、完成表を持つ状態から
乗換コストを17へ変更する更新中の比較条件を用意した。
通常起動の自動初期構築、更新中、旧方式を分けて評価する。

### 回帰

MSVC x64 Release、assert ON、LTO OFF、通常製品のPassenger Benchmark OFFで全CTest **628/628成功**。
事前計算の通常回帰は**22ケース・19,886アサーション**。
測定用翻訳単位をすべて外しPassenger Benchmark OFFへ戻した後も、関連する通常回帰・製品起動試験**25/25成功**。
初期取り込みから全行完成までStateTicksが進まないこと、全行置換中の到達性、任意カーソルからの保存復元、
構造変更中の進捗維持・安全な別系統の継続、v6保存の移行、公開ごとの旧行解放、
小さいPacketIDの新規客より先に待っていた集団の部分予約・積載を確認した。

合成CPU計測では、同じ系統の候補が128 / 1,024 / 8,192 / 32,768 Packetのとき、
先頭の乗車可能Packetの取得は196 / 191 / 191 / 199 ns、検査数は毎回1件だった。
この試験は対象外Packetを別系統へ置く以前の試験に加え、**対象系統自体に大量の候補がある条件**を試す。
全合成試験のprocess CPUは2.797秒、OS peak working setは48.16 MiB。個々の表だけのpeakではない。

### 実マップ

前回と同じ補正済み固定入力（SHA-256 `d8d40bd49ca5fbb20c2f9645d984a2b19f41eb7b482bf6707e49da8fd61ee1ea`）、
同じ凍結素材、各ビルドに対応する英語ファイルを使用した。
いずれもStateTicks **3,181,092,957 → 3,181,093,981**の1,024実tickで、会社の同じ経済期間内の配送差分を取る。
比較の旧方式は同じ新版バイナリで事前計算を無効にした条件。
ビルド・テスト・他OpenTTDとの競合検査は空だった。各条件1回であり、その他のホスト活動や実行順の影響までは排除していない。

| 条件 | simulation平均 ms | p95 ms | p99 ms | Journey平均 ms | 全process CPU 秒 | OS peak GiB | 全旅客配送 人 | 終了時待機 人 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 旧方式 | 50.13 | 80.52 | 101.82 | 21.03 | 132.73 | 2.04 | 49,368 | 483,037 |
| 新版・通常起動の自動準備 | 29.37 | 51.37 | 72.15 | 4.92 | 163.80 | 5.24 | 49,302 | 479,887 |
| 新版・旧表を維持して更新中 | 40.38 | 66.64 | 87.08 | 15.88 | 167.33 | 5.23 | 49,669 | 493,972 |

自動準備後のsimulation elapsedはこの旧方式条件より41.4%短く、更新中は19.4%短かった。
**全process CPUにはロード・初期構築・保存を含み、自動準備条件では23.4%増えた。**
通常tickだけのOS CPU時間や、初期構築だけのprocess CPUを独立に計測した結果とは区別する。
更新中は明示的な乗換コスト変更（20→17）を含むため、輸送結果を同じ経路コスト条件の品質比較とは扱わない。

自動準備はsimulation開始前に4,636行を完成させた。準備sliceの合計elapsedは35.31秒、主要配列は3,278,430,532 bytes。
更新中は新251行・旧4,385行を保持し、主要配列は新184,378,692 bytes + 旧3,101,284,772 bytes = **3.06 GiB**。
新表構築は67,108,864操作・elapsed約2.54秒で、旧行を解放しながら進んでいる。
自動準備後・更新中の両条件で、通常のPeriodic Timed全国検索と旧Active全国検索は0回だった。
Packet単位の再評価数、全実マップでの列車到着時候補検査数は今回のログには追加していない。

### 実セーブの往復

自動準備条件の終了セーブと、更新途中の終了セーブをそれぞれ0tickで再ロードした。
両方でStateTicks、Packet数、貨物人数、既知OD人数、Journey Packet数、Packet保存フィールドのdigest、支払いdigestが一致した。
更新中の新251行・旧4,385行と67,108,864操作も再現した。
再ロードのprocess elapsedは完成済み41.48秒、更新途中42.92秒であり、ロード時再生の長時間化は残っている。
これは実server/client同期の代わりにはならない。

### 待機人数増加の原因を切り分ける対照実験

同じ新版で、初期表を従来の `--warm-precomputed` で完成させ、乗車優先順だけを変えた。
`packet_id_control` サブコマンドはコピーした測定用CargoPacket翻訳単位だけを変更し、
旧 `std::set<CargoPacketID>` と同じPacketID優先順を復元する。経路表・指令・設定・固定入力は揃え、
候補の直接走査も両条件で使う。製品ソースを旧順序へ戻す変更ではない。

| 同じ初期表の条件 | 終了時待機 人 | 全旅客配送 人 |
|---|---:|---:|
| PacketID順の対照 | 503,871 | 49,573 |
| 主キュー順FIFO | 485,296 | 49,407 |

**PacketID順の対照は、前回の待機503,871人だけでなく、Packet保存フィールドと支払いのdigestまで完全一致した。**
FIFOへの変更で待機が18,575人減り、前回の旧方式に対する増加20,834人の約89%が解消した。
これは今回の短期条件では、待機増加の大部分にPacketID優先の乗車順が寄与していたことを示す。
番号が再利用される新規Packetに、既存の待機客より先の優先権を与える構造を取り除いた。
一方、FIFOの配送人数はPacketID対照より166人少なく、待機人数だけで全輸送品質を評価しない。

通常起動では初回の系統取り込みを完了してから入力を凍結するため、同じ新版の計測専用warmと初期入力も異なる。
通常起動の待機は479,887人でFIFO warmよりさらに5,409人少なかった。
通常起動の旧方式に対する配送差は−66人（−0.13%）、待機差は−3,150人。
更新中の配送は旧方式より301人多い。CPU時間の短縮を、配送人数の大幅削減だけによる成功とは判定していない。

実乗車距離・最終所要時間・同駅再訪・OD別の反復失敗・列車種別分散の長時間観測は未実施。
既存のNewGRF容量変更警告も残っており、この1,024tick比較から長期品質やMP安全性を推定しない。

### 実server/clientの途中参加：失敗

`utils/passenger_precomputed_join.py` で、完成済みの自動準備条件の終了セーブをサーバーへロードし、
手動pauseを保持したまま `127.0.0.1:17529` から同じバイナリのクライアントを参加させた。
サーバーのロード後Packet・支払いdigestは元セーブと一致した。
しかし、クライアントは参加を完了できず、サーバーは次を記録した。

```text
Client #2 is dropped because it took longer than 500 ticks to join
routing-client has left the game (processing map took too long)
```

既定の `max_join_time=500` を変更していない。
試験スクリプトの120秒は観測の打切り期限であり、ゲームのネットワークtimeoutを延ばすものではない。
クライアント側の比較snapshotは取得できなかったため、Packetの不一致を検出した結果とは区別する。
公開表の同期的なロード再生が残り、既定期限内の途中参加を成立させる要件は未達。
ロードの各段階とネットワーク受信の停止時間を独立にプロファイルしたとは主張しない。

必須ケースのうち、実ファイルでの完成表保存・再ロードは確認した。
完成表をロードしたサーバーへの参加は失敗し、初期構築途中・更新途中の参加、ダイヤ変更後、
継続する乗降・予約・分割・結合を伴う実MP同期は未検証である。
今回の変更をMP対応済みとして統合しない。

## コミット

- Phase 1：`8ed57ec457`（初期準備・旧行維持・v7保存）、`a39602b5c7`（両固定入力のメモリ計上）。
- Phase 3：`25bd5c44cc`（候補の直接走査と主キュー順）。
- 合成・更新計測：`fc7718fe65`。
- Phase 5の対照実験：`00a0742b33`。
- Phase 6の実途中参加試験：`c90f36f00d`。

Phase 2・4の実装コミットは存在しない。Phase 5・6の試験追加を、対応する全要件の実装完了として扱わない。

## 根拠と再現

- [全条件のsummary.json](measurements/passenger-precomputed-completion-2026-10-15/summary.json)：実tickの平均・p95・p99、Journey時間、各種輸送カウンタ。
- [provenance.json](measurements/passenger-precomputed-completion-2026-10-15/provenance.json)：固定入力・バイナリのSHA-256、保存往復と前回対照一致の検査。
- [合成CPU](measurements/passenger-precomputed-completion-2026-10-15/cpu/result.json)、[全CTest](measurements/passenger-precomputed-completion-2026-10-15/checks/build-ctest-completion.log)、[通常製品へ復帰後](measurements/passenger-precomputed-completion-2026-10-15/checks/build-restored-tests.log)。
- [乗車順だけを変えた対照patch](measurements/passenger-precomputed-completion-2026-10-15/packet-id-control/control.patch)。
- [実途中参加の失敗](measurements/passenger-precomputed-completion-2026-10-15/join-complete/result.json)。server/clientのconsole・圧縮stderr/stdoutを同じディレクトリに保存した。

各実マップディレクトリにconsole、実tick標本、process CPU/peak、Packet・支払いsnapshotを残した。
実セーブ、素材、実行ファイルはGitへ追加しない。
実マップ計測用は従来の `prepare` → `stdio` → ビルドを使用し、次の条件を順番に実行する。

```text
python utils/passenger_precomputed_real_map.py run <measured-exe> <package> <legacy> --ticks 1024 --no-precomputed --input <canonical.sav> --lang <lang>
python utils/passenger_precomputed_real_map.py run <measured-exe> <package> <automatic> --ticks 1024 --precomputed --input <canonical.sav> --lang <lang>
python utils/passenger_precomputed_real_map.py run <measured-exe> <package> <replacement> --ticks 1024 --precomputed --rebuild-precomputed --input <canonical.sav> --lang <lang>
python utils/passenger_precomputed_real_map.py run <measured-exe> <package> <warm> --ticks 1024 --precomputed --warm-precomputed --input <canonical.sav> --lang <lang>
python utils/passenger_precomputed_real_map.py run <measured-exe> <package> <reload> --ticks 0 --resume --input <saved-result.sav> --lang <lang>
```

対照用は、準備したビルドに `packet_id_control <source> <build>` を適用して別名バイナリへ保存し、
`--warm-precomputed` で実行する。通常製品へ戻す際はCMakeを再configureしてコピーTUの参照を除去し、
`OPTION_PASSENGER_BENCHMARK=OFF` で再ビルドする。
実途中参加は `python utils/passenger_precomputed_join.py <measured-exe> <package> <complete-save.sav> <output> <lang>`。
この実行は成功判定を返しておらず、上記の失敗を再現するためのWindows用試験である。
