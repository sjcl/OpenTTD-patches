# Work Admission: 最新ベースへの追従と統合検証

**最終の統合対象はローカル `jgrpp-ya` の `84ab414e6b771a82444310c58241b72ae2f9a007`、検証した製品ソースは `4a4c06157dae72fd0dd9dad3d177a76e6b0aaebf`。** リモート未反映だった6コミット（街の自動資金供給、車両グループのマーカー等）も保持して追従した。未コミットの `vcpkg.json` は変更していない。性能比較はユーザー指定により省略した。

[最終検証の証拠](measurements/passenger-query-work-admission-2026-10-15/latest-main/result.json)で、Benchmark OFF・assert ON・LTO OFFの製品ビルド、**全CTest 606/606、旅客364ケース・2,671,581アサーション**、受付拒否共有49アサーションが成功した。Work配分、Packet結合、ネットワーク処理に追加変更はない。Roundsも取り込んでいない。

最新mainの保存形式を含む実ファイルのロード・再保存・再ロード、同じ入力からの512tick継続も成功した。cache/Scratch有効と全無効で、Work・拒否・結果・実輸送・全保存対象フィールド・全Save SHA-256 **`4039450019e02e3ad34ac5f27fa18e5a720afed4f4205c83ff3509cf2512c474`** が一致した。受付8,118・実Work140,426,084・返却391,595,164など下記512tickの論理集計も一致。旧v4のPacket/支払の保全、v3再保存・再ロード、256tick継続も再確認した。

通常cacheのloopbackは初期・途中参加成功後300秒、StateTicks **3176545487 → 3176552132**（全体6,645tick）進行し、両clientが接続を維持。終点で3台の194,879 Packet・859,965 cargo単位・既知OD284,754単位・Journey20,176 Packet、全保存対象フィールド/支払digestが一致した。支払検証成功、desync・assert・切断・異常終了・競合の検出0。受信timeoutの延長は行っていない。

追加GUI文字列に伴い言語packの版が更新された。実マップは同じSave・NewGRF・baseset・configを使い、言語packだけ現在の生成物へ揃えた。旧 `Release/lang` で製品起動の保存テスト3件が失敗した原因は、追加ログの「not a valid language file」で分離した。更新後の製品構成で606件全てが成功した。計測用の実tickドライバーを使った構成で通常AI回帰3件が失敗した試行も、製品の成功結果へ読み替えず原本を保持した。

**最新mainへの統合可。** 機能・保存・同期・Packet結合と製品回帰を根拠とし、未実施のCPU比較を合格理由にはしない。以下は `a6e6664a18` へ最初に追従した段階の詳細であり、その数値・原本も残す。

対象は `feat/passenger-query-work-admission`。比較ベースは `jgrpp-ya` の `a6e6664a185a1a49c1f0df1ecc2572e70762f3b7`、検証した製品ソースは `cb9bef900d7b529c9e21e854b59f084b57db6fb7`。上流のFallback乗車・via表示・routing無効時のreturnとdirect ride保持、Journey採用時の更新済みhop保持を取り込んだ。Roundsは取り込んでいない。

最初の追従先 `7753377108` での600件の回帰と一部性能ラン完了後、統合先に `a6e6664a18` が追加された。保存状態にも影響する修正なので、その時点の未完了比較は統合基準へ使わず、新しいベースで再検証した。

その後、ユーザーから「性能比較はスキップしていいです」と指定されたため、新ベースのA/B/C CPU比較を中止した。新ベースの平均・p95・p99、輸送性能・メモリ・Packet最大数の比較は未測定であり、CPU退行がないという実測の結論は出さない。旧ベースの部分測定は[参考原本](measurements/passenger-query-work-admission-2026-10-15/superseded-7753377108/status.json)として保持し、現行版の比較結果へ流用しない。

上流mergeで競合したPeriodic受付処理は、Work予約・実使用精算を保持し、拒否時の `fallback_hop` と同一イベントの拒否理由共有を組み合わせた。追加回帰は、同じODの別PacketにQueryLimit拒否を共有しても追加Work/RNGを使わず、最新のCargoDist hopを保持することを確認する。

上流のSave/Load回帰には、`StationCargoList::Append()` がhopを更新する一方で旧invalid hopを期待するfixtureがあった。実際の保存キュー復元と同じく、fixture構築後に保存フィールドを戻す形へ修正し、旧invalid hopと現行B/C hopの両方をround trip・AfterLoadで検証した。製品の保存処理をテストに合わせて変更していない。

16 OD/tick、Timed 49,152 / Schedule保護16,384、1 OD 65,536、Global 1,048,576 Work/tickは維持した。最新比較ベースに対して、Snapshotの `AdmitQuery()`、Packetの結合条件、ネットワークの製品コードは変更していない。

全キャッシュ・Scratch無効での受信タイムアウトは、ユーザー指定により[独立したネットワーク堅牢化課題](passenger-network-receive-fairness-followup.md)として扱う。[原因分離の失敗原本](passenger-query-work-admission-network-cause-2026-10-14.md)は保持する。タイムアウトを延長せず、通常cache有効のプレイ・途中参加と、offlineでのcache状態による決定性をこの統合の条件とする。

## 回帰とSave/Load

MSVC x64 Release `/O2 /Ob2 /MT`、assert ON、LTO OFF、`OPTION_PASSENGER_BENCHMARK=OFF`で製品・テスト・regression_filesをビルドした。全CTest **602/602**、旅客回帰 **364ケース・2,671,581アサーション**が成功した。受付拒否・Fallback hop・同一OD共有の追加回帰49アサーションも成功した。Work予約の例外/早期return・部分予約・各上限・Fair Share・配分・split/merge・各輸送方式・車庫/運転停車/強制降車・保存/同期の既存回帰を含む。

年齢計算の境界回帰も上記に含む。`GetPassengerScheduleIntentAge()` の実装は `c16da03990` から変更しておらず、既存の327アサーションとGCC UBSan trapモード95ケースの証拠は[前回レポート](passenger-query-work-admission-final-2026-10-14.md)に保持する。今回新たなUBSan実行を主張しない。

固定入力SHA-256は `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`、16,384×4,096の実マップ。実ファイルでロード→v3保存→0tick再ロードを確認し、同じ再保存からStateTicks **3176545487 → 3176545999** の512tickを通常cacheと全derived cache/Scratch無効の2条件で継続した。Packetの全保存対象フィールド・OD・Intent・支払情報、人数、経路結果集計、実輸送が一致した。

2条件の全Save SHA-256は **`98c1b98040497b9dcfb7d316cee333a11e36c2795f06bb5f81f8441da9b44ccb`**、27,928,284 bytesで一致。終点は107,890 Packet・843,588 cargo単位・既知OD134,757単位・Journey6,330 Packet。支払検証成功、Error pause解除0。さらに通常条件の終点Saveを0tick再ロードし、Packet/支払情報の保全を確認した。warmでpolicy hit 1,259・Scratch保持867,536 bytes、coldでpolicy hit 0・Scratch保持0、全無効化コマンドの応答を確認し、実際に異なるcache状態を検証した。

| 512tickの論理集計（2条件で一致） | 値 |
|---|---:|
| 受付OD / 拒否 | 8,118 / 69,465 |
| 予約 / 実使用 / 返却Work | 532,021,248 / 140,426,084 / 391,595,164 |
| 最大受付 / 最大実Work（tick） | 16 / 916,069 |
| Timed配分 / 使用 | 399,015,936 / 101,817,743 |
| Schedule配分 / 使用 | 60,466,147 / 38,608,341 |
| Timed Certified / Provisional | 5,284 / 447 |
| Work終了 / Work後も両経路なし / Schedule救済 | 1,098 / 652 / 1 |
| Work終了率 / 両経路なし率（受付ODに対する割合） | 13.525% / 8.032% |
| Timed乗車 / 乗換 / 配送 | 5,225 / 29 / 85人 |
| Schedule乗車 / 乗換 / 配送 | 28 / 1 / 0人 |
| Untimed乗車 / 乗換 / 配送 | 43,383 / 19,793 / 8,914人 |
| 全方式配送 | 8,999人 |
| 終点の既知OD待機 / Schedule intent age ≥4,096 | 68,669 / 0人 |

受付・拒否理由・配分・Workの両精算式・Timed/Scheduleの論理Work・結果・tick別件数/Work histogram・輸送の一致を確認した。これらは決定性と実処理の検証値であり、旧課金への性能・輸送改善比較ではない。待機人数は新しく生成されたODも含み、Schedule intent ageは再計画でリセットされる。Timed旅客の連続待機時間や未達旅客の固定cohortとは扱わない。廃止したWork履歴方式の乗車/配送値は使っていない。

## 旧v4実ファイル

入力v4 SHA-256 **`2fa1b07407db3ca3077664fb9aab21a13378d2df6d478c80d4cf81302495f1c2`** を現行の未知フィールドskip経路で再ロードした。廃止タグtrue 11,703 / false 182,080 Packetを含む既存fixtureであり、計193,783 Packet・855,403 cargo単位・既知OD299,288単位・Journey19,802 Packet、全保存対象フィールドと支払digestが以前の一時reader観測と一致した。

実ファイルのSLXIを独立に読み、v4入力→v3再保存→v3再ロードを確認した。再保存から256tickの実運転、支払検証、Error pause解除0も成功した。Work履歴のメンバー・保存フィールド・結合条件を製品へ戻していない。異なる履歴の225組が通常merge条件を満たした前回の観測と、最新比較ベースからCargoPacket/保存tableにWork Admission差分がないこと、今回の保存/merge回帰を併用する。

## 通常cacheの途中参加

[loopback原本](measurements/passenger-query-work-admission-2026-10-15/multiplayer/result.json)。同じ実入力のserver1台・初期client1台・途中client1台、`sync_frequency=1`、通常CPU affinity、policy・departure index・fallback cache・Scratchを全て有効にして実行した。両参加が完了し、途中参加後300秒、serverのStateTicks **3176545487 → 3176552316**（全体6,829tick）まで3台が進行した。

両clientが終点でも接続を維持。同tickで197,225 Packet・862,256 cargo単位・既知OD289,133単位・Journey19,339 Packet、全保存対象Packetフィールドdigestと支払digestが3台で一致し、支払検証が成功した。途中参加直後のclientは保存された実行状態をロードしてderived cacheを再構築するため、serverとcache履歴が異なる条件も含む。同期エラー、assert、切断、異常終了は検出されず、競合プロセスの観測も0だった。

受信監視20秒、ハーネス既存の `max_lag_time` / `max_join_time` 32,000は変更していない。観測終了後にserverをpauseして同tickの状態を照合した。継続観測を途中でpauseして接続失敗を隠す方式は使っていない。既存NewGRFのcapacity警告を含む完全stderrをgzipで保持する。警告文の「desyncs and/or crashes」を実際の同期エラーと混同せず、実エラーとCargo状態を照合した。

## 統合判断

ユーザー指定の性能比較省略を前提として、最新ベースへの追従、通常cacheの実運転・途中参加、全cache/Scratch無効とのoffline決定性、v3/v4保存互換、Packet結合、回帰の条件を満たした。Work配分・受付上限を増やしておらず、Roundsやネットワークの動作変更は含めない。`jgrpp-ya` への統合可と判断する。CPU・p95/p99・輸送改善の新しい比較実測を合格理由には使わない。

[統合検証・製品構成・回帰の証拠](measurements/passenger-query-work-admission-2026-10-15/result.json)。製品構成へ戻す際にCMakeを再生成し、コピーした計測TUのobjectを明示的に除去して `OPTION_PASSENGER_BENCHMARK=OFF` で再ビルドした。製品プロジェクトに計測TU参照や計測defineが残らないことも確認した。検証後の変更はスクリプト・レポート・原本の整理のみで、製品ソースは上記SHAから変更していない。

## 再現手順

既存の `docs/measurements/passenger-query-work-admission-2026-10-14/` のスクリプトを利用する。新しい証拠は `measurements/passenger-query-work-admission-2026-10-15/` に保存する。Save・実行ファイル・NewGRFそのものはGitへ追加せず、SHA-256と小さな原本ログを残す。以下の最初の3行は中止した性能比較の再現手順で、今回の採用結果には含めない。

```text
Build-Variants.py build-admission --variants control q16-t49152 --control a6e6664a18 --final-metrics
Run-Final-Review.py build-admission <frozen-package> --output integration-2026-10-15-comparison
Summarize-Final-Review.py <comparison>/sequence.json --archive <evidence>/profile
Run-Integration-SaveLoad.py build-admission <frozen-package> <fixed-input>
Check-Integration-Cache.py <save-review>
Run-Integration-V4.py <save-review> <frozen-package> <retired-v4-save>
Prepare-Save-Integrity.py build-admission
Prepare-Network-Probe.py <source> build-admission
Prepare-Final-Multiplayer.py build-admission <frozen-package> --directory integration-2026-10-15-mp-warm --warm-clients
Run-Final-Multiplayer.py build-admission <fixed-input> --duration 300 --directory integration-2026-10-15-mp-warm
Archive-Final-Network.py <mp-run> <evidence>/multiplayer --outcome pass --run-log <mp-log>
Archive-Integration.py build-admission <evidence> --skip-performance
```

計測・Save/Load・loopbackは共有ロックで直列実行し、他ゲーム・ビルド・テストと同時実行しない。各性能ラン前90秒の無競合待機と実行中5秒ごとの監視を使う。最終段階でCMakeを再生成して `OPTION_PASSENGER_BENCHMARK=OFF` へ戻し、コピーしたprobeのobjectを明示的に除去して製品をビルドする。

最新mainの再検証では、`Prepare-Integration-Package.py <build> <frozen-package>` で現在の言語packを固定した。Save検証の `--directory` は `integration-2026-10-15-saveload-latest-main`、途中参加は `integration-2026-10-15-mp-warm-latest-main` を使用し、同じ5段階のSave検証・3段階のv4検証・cache会計照合・300秒観測を再実行した。証拠保存は以下の引数で行った。

```text
Archive-Integration.py build-admission <evidence>/latest-main --skip-performance --save-directory integration-2026-10-15-saveload-latest-main --base-commit 84ab414e6b
```
