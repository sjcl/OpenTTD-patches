# Timed JourneyのAnytime採用

ベースは `c2c4d1898c3a38a091ff224c5be660ede72238f7`、ブランチは `feat/passenger-timed-anytime`。
観測だけを追加した `3f66b40dc0` のバイナリを先に固定し、旧pop-only方式の実マップ診断と、Anytime採用後の実マップ測定を分ける。
既存の比較基準は [632399cfc1の8,192tick報告](https://github.com/sjcl/OpenTTD-patches/blob/632399cfc1e6c0c9125a58e1f2c9a122175f79ba/docs/passenger-routing-profile-c2c4d1898c-2026-10-13.md)。


最終版では、8,192tickの各反復で **4,854検索を未popの完全経路から救済**し、Provisionalを **5,713 packet・延べ6,078人**の実際のTimed ServiceIntentとして採用した。
Timed最終配送は同じ世代検証を使う旧pop-only対照の16,362人から17,876人へ9.25%増えた。全方式の最終配送の合計は241,766人から241,896人で、ほぼ横ばいだった。
保存・キャッシュ無効・途中参加・製品版580テストを確認した。ただし暫定経路は最適性を保証せず、品質標本には参照より到着予定が25,199tick遅い例がある。

## 最終計測：世代検証を共通にした対照との比較

実装の測定元は `b57532a4a2` と、src/CMake差分SHA-256 `ee54dfe6a0a74ecc7eafcead2d60a5869373e9080f2a70eab9bf1dcd2ef47f0e`。
追加した世代検証後の実マップ7ランを[最終資料](measurements/passenger-timed-anytime-2026-10-14/final-validated/summary.json)に保存した。
旧pop-only対照とAnytimeは同じ完全チェーン・Service世代検証を使う。元方式そのものの観測は別の `original` ランに分けた。

| 8,192tickの検索・採用 | 旧pop-only対照 | Anytime各反復 |
|---|---:|---:|
| Timed検索 | 131,072 | 131,072 |
| Certified | 46,267 | 53,511 |
| Provisional | 0 | 4,854 |
| 未pop候補からの新規救済 | 0 | 4,854 |
| Deferred | 59,875 | 38,910 |
| NoRoute | 11,866 | 16,937 |
| Unavailable | 13,064 | 16,860 |
| Work終了・有効な完全候補あり | 33,944 | 4,820 |
| Work終了・有効な完全候補なし | 25,790 | 38,817 |
| Provisionalのpacket採用 | 0 | 5,713 |
| Provisional採用の延べ旅客数 | 0 | 6,078 |

Work終了の合計は59,734検索から43,637検索。AnytimeのProvisional 4,854件はすべて未pop候補からの新規救済で、そのうち4,820件がWork、34件が他の上限制約による結果だった。
FoundにはCertifiedだけを数え、Provisionalの打ち切りReasonを保持する。検索、packet、旅客数はそれぞれ延べ件数で、同一ODや同じ旅客の再計画・cache採用を含む。
採用が実運転を変えるため、旧方式で保持していた候補数と、Anytimeで実際に救済した数は同一の問い合わせ列の比較ではない。

| 実輸送（人、8,192tick） | 元方式の観測 | 共通検証の旧pop-only | Anytime各反復 |
|---|---:|---:|---:|
| Timed 乗車 | 43,837 | 51,599 | 56,412 |
| Timed 乗換 | 20,724 | 23,395 | 26,434 |
| Timed 最終配送 | 12,998 | 16,362 | 17,876 |
| Schedule-aware 乗車 | 10 | 28 | 88 |
| Schedule-aware 乗換 | 10 | 24 | 26 |
| Schedule-aware 最終配送 | 0 | 0 | 36 |
| Untimed 乗車 | 558,057 | 550,498 | 543,419 |
| Untimed 乗換 | 276,375 | 273,135 | 267,082 |
| Untimed 最終配送 | 229,971 | 225,404 | 223,984 |

各値は実際のCargo移動カウンター。計画数を配送数とは扱わない。複数モードの利用を示すSchedule-assisted final ODは最終運搬モードの配送と重複するため、合計へ追加しない。

## 最終計測：性能と決定性

| 指標 | 共通検証の旧pop-only | Anytime 1回目 / 2回目 |
|---|---:|---:|
| 探索Work平均/OD | 35,420.44 | 28,032.62 / 28,032.62 |
| Work p95上端 | 65,536 | 65,536 / 65,536 |
| Timed検索時間/OD (ms) | 0.745 | 0.663 / 0.673 |
| プロセスCPU時間 (s) | 424.266 | 420.359 / 420.688 |
| simulation平均 (ms/tick) | 37.000 | 36.374 / 36.344 |
| simulation p95上端 (ms) | 57 | 56 / 56 |
| simulation p99上端 (ms) | 75 | 71 / 71 |

探索Work平均は20.86%減、simulation平均は2反復平均で1.73%減。この対照は1反復、Anytimeは2反復なので、速度差は本マップ・本環境での観測値とする。
CPU時間は読み込み・保存・workerを含む。simulation平均は実際に進んだtick、p95/p99は1ms bucketの上端。品質probeと継続区間は初回の性能比較へ混ぜない。

Anytimeの2反復と、Timed/Schedule policy・Departure Index・resident fallback cacheを無効にした8,192tickは、論理Work・結果状態・採用・実輸送・最終Saveが一致した。
最終Save SHA-256は `d8b8b8374eba20ef5d00faa8508baf20fc0865ae851f6ff5cf914a4d2a328c11`。
キャッシュ無効時のsimulation平均は56.761 ms/tickだが、速度の最適化と経路判断を分離できている。[一致確認](measurements/passenger-timed-anytime-2026-10-14/final-invariance.json)を保存した。

## 最終計測：元方式の利用可能候補の観測

元方式の131,070検索のWork終了66,426件では、目的地に届いたLabelを持つ検索が37,595件、そのうち現在の世代で接続した完全チェーンを持つ検索が31,748件だった。
利用可能な完全候補の割合はWork終了の47.79%。queue残存と改善可能な下界は31,748件、pop済み有効候補は0件。
消費した逆探索Workは880,957,611、前向きWorkは3,472,336,725で、各Work終了検索の和は65,536だった。
費用・到着予定時刻の16例と停止地点は[最終original観測](measurements/passenger-timed-anytime-2026-10-14/final-validated/original-minimal-on-1/console.txt)に保存した。

元方式の探索Work・実輸送は既存基準と一致した。保存形式のheaderと、展開した882,410,938 bytesの全データも、記録された現在のビルドrevisionの等長文字列2か所だけを除いて一致した。
そのため圧縮後のSaveハッシュの違いはビルド記録によるもので、診断によるゲーム状態の変更ではない。[比較証跡](measurements/passenger-timed-anytime-2026-10-14/original-save-comparison.json)に両Save・正規化payloadのハッシュを保存した。[比較スクリプト](measurements/passenger-timed-anytime-2026-10-14/Compare-Original-Payload.py)で再検証できる。

## 最終計測：暫定結果の品質

2,048tick・32,768検索で、1021問い合わせごとの系統標本33件と、Provisionalの64件ごとの追加標本23件を採取した。
同じOD・cargo・ready・除外Service・ライブ状態で、同予算の旧pop-onlyと、高Work参照（Work 1,048,576、Labels 65,536、Memory 128 MiB）を比較した。
56標本で同予算Workが一致し、旧Certifiedの15標本の費用・到着予定時刻は変わらなかった。救済26件はすべて旧方式が経路を返せず、参照はCertifiedだった。

| 暫定経路とCertified参照の差 | 系統標本の暫定3件 | 追加暫定標本23件 |
|---|---:|---:|
| 費用差 中央値 / p95 / 最大 | 0 / 25,179 / 25,179 | 0 / 1,504 / 1,553 |
| 到着予定差 中央値 / p95 / 最大 (tick) | 0 / 25,199 / 25,199 | 0 / 1,524 / 1,573 |

暫定採用は大きく劣る候補も返し得る。Provisionalを最良経路の証明として扱わず、乗換後は現在時刻から再計画する。
この系統標本と追加標本の層を混ぜた平均を、全ODの品質保証とは解釈しない。予定到着の差は実際の旅客の遅延測定ではない。
個別のOD・ready・費用は[最終品質JSON](measurements/passenger-timed-anytime-2026-10-14/final-quality-summary.json)と[標本JSONL](measurements/passenger-timed-anytime-2026-10-14/final-quality-summary.jsonl)に保存した。

## 最終計測：保存後の継続・途中参加・製品版

最初の8,192tickを保存し、そのSaveからさらに8,192tick、合計16,384tickを進めた。
StateTicksは `3176545487 → 3176553679 → 3176561871`。継続区間の最終Save SHA-256は `ccf4492754265d5caeefa349ecf89b81fec96d8a0bcada57ec788ed18e67418c`。
継続専用driverはゲーム読み込み時のログから最初のStateTicksを取り、pauseや読み込み待ちのdriver loopsをsimulation ticksと混同しない。
Error pause解除はこの最終ランで0回だった。[実clock](measurements/passenger-timed-anytime-2026-10-14/final-continuation-clock.json)にdriver loopsも記録した。

| 継続区間（追加8,192tick）の実績 | 件数 |
|---|---:|
| Timed検索 / Certified / Provisional | 131,072 / 39,791 / 3,939 |
| 未pop候補からの新規救済 | 3,939 |
| Provisional採用 packet / 延べ旅客 | 5,949 / 6,585 |
| Timed実乗車 / 乗換 / 最終配送 | 52,961 / 34,146 / 18,150 |
| Schedule-aware実乗車 / 乗換 / 最終配送 | 49 / 51 / 6 |
| Untimed実乗車 / 乗換 / 最終配送 | 692,412 / 395,535 / 281,535 |
| Timed失効 packet / 延べ旅客 | 49,129 / 83,151 |
| 同じServiceでの実乗車検証失敗 | 7,265 |

失効・再探索はTimed全体の延べ観測で、再試行が重複する。保存へProvisionalタグを足さないため、採用後の失効を暫定経路由来だけに分離した数値ではない。

実マップのloopback試験では、初回と途中参加の2クライアントが終了時も接続し、serverは1,055tick進んだ。
clientの最終観測はserverから44/18tick以内（観測間隔128tick）で、policy・Departure Index・fallback cache・scratchを無効にしたまま進行した。
sync frequency 1で同期エラー・assertion・切断は検出されず、serverはProvisionalを949 packetに実採用した。
[途中参加結果](measurements/passenger-timed-anytime-2026-10-14/final-multiplayer-result.json)に進行・接続・cache設定を保存した。

最後に計測処理を外したMSVC x64 Releaseをビルドし、assert ON・LTO OFF・`OPTION_PASSENGER_BENCHMARK=OFF`で **580テストが全件成功**した。
旅客回帰360ケース・2,725,303アサーション、計測スクリプト8件とprofileスクリプト23件も成功した。
[製品版検証](measurements/passenger-timed-anytime-2026-10-14/final-product-verification.json)、[CTest](measurements/passenger-timed-anytime-2026-10-14/final-ctest.log)、[旅客回帰](measurements/passenger-timed-anytime-2026-10-14/passenger-final-tests.log)を保存した。

継続TUのヘッダー順序、読み込み前のclock、ビルド途中に採取したmanifestの問題を修正した。
古いsnapshotの継続ランを除外し、バイナリを固定した別manifestで再計測した。失敗・除外した試行は[除外記録](measurements/passenger-timed-anytime-2026-10-14/excluded-attempts.json)に保持する。
入力NewGRFの既存容量変更警告も各stderr証跡に保持した。製品の容量・予約・pause・実乗車検証は変更していない。

## 探索結果と採用

`PassengerPeriodicPlan::state` は検索結果だけに保持する。`Reason` は打ち切り理由、`observations` は重複し得る観測理由として引き続き使う。

| state | 意味 | Timed経路採用 |
|---|---|---|
| Certified | 従来の下界・省略候補判定で、公開されたTimed探索領域の最良完全経路を確認 | 可 |
| Provisional | 完全経路を保持しているが、同費用のタイブレークを含め、残る探索から改善できないことを未証明 | 可 |
| Deferred | Work・Label・Memory・乗換安全上限で、有効な完全経路を返せない | 不可 |
| NoRoute | 所定の公開Timed探索領域で経路なしを証明 | 不可、Schedule-aware/Untimedは別途判定 |
| Unavailable | 無効な問い合わせ、未対応評価、未知の位相等で結論を出せない | 不可 |

Provisionalの`deferred`はtrueのままとし、`Reason::Found`には変更しない。既に目的地Labelをpopしたが探索を証明まで完了できなかった従来の結果もProvisionalに分類する。新たに救済できた検索は、その従来のpop済み候補とは区別して `rescued_unpopped` に数える。

Label挿入は、既存の支配判定とWork/Label/Memory admission、Label格納、完全候補の記録、queue挿入の順。
候補のキーは費用、到着時刻、乗車回数、最初の発車時刻、ServiceID、乗車Order Index、降車Order Index。
同じキーでは最初に格納された候補を保つ。有効なServiceについては、既存の枝刈りと、queueをpopするまでのWork・探索順序を保つ。

追加の世代検証では、無効な安い目的地候補が有効な候補を隠したり、枝刈りの上限になったりしないよう、現在のServiceによる完全チェーンだけを候補として保持する。
前向き探索でも、無効化済み・削除済み・incarnation不一致のServiceを除外する。この除外は元方式の問い合わせ列やWorkを変え得るため、元の挙動を観測する `PlanWithOriginalCompletion()` と、同じ世代検証を共有する `PlanWithLegacyCompletion()` を別に測定する。
同予算での探索順序・Work一致と旧Certifiedの経路一致は、後者との比較で確認する。

返却前に、目的地Labelから親indexが厳密に減少するチェーンをたどる。区間の乗降駅、乗車回数、時刻、Serviceのincarnation/generation、Origin/readyのroot、最初の区間との一致を確認する。
部分Label、admissionで拒否したLabel、親チェーンを復元できない候補を採用しない。

`PlanWaiting()`は検索未完了とpacket採用を別に扱う。イベント内cacheはProvisionalと未完了フラグを保持するが、rideがあれば通常のTimed ServiceIntentを採用する。経路がなければ従来どおりSchedule-awareを残りWorkで試し、必要ならpacketを延期する。

## 実乗車と保存

CargoPacketへ保存するのは従来と同じ、次の一乗車区間だけ。Provisional状態や完全な旅程を保存しない。CargoPacketの形式・サイズ、JPSDの保存version、ネットワーク同期形式は変更しない。

実際の予約・乗車・継続は `ValidatePassengerServiceRide()` と既存のCargo移動処理を通る。Serviceのincarnation/generation、駅とOrder Occurrence、CargoType別NoLoad/NoUnload、途中の強制降車、最低乗換時間、車両容量・実空席・予約、車庫・Waypoint・運転停車、遅延中の実車の扱いを変更しない。
乗換後は実際の現在時刻と到着Serviceから再計画する。Provisionalの先行旅程はその先の乗換や座席を保証しない。

採用する候補は整数費用と保存されたIDの順で決まる。壁時計、診断カウンター、policy/次発index/Scratchの常駐状態は判断に使わない。旧方式は `PlanWithLegacyCompletion()` に残し、ローカル設定で製品アルゴリズムを切り替えられるようにはしない。

## 診断の範囲

`passenger_journey_stats total` に、最終ReasonがWorkの検索について完全Labelの有無、queue残存・pop済み候補の有無、改善可能な未探索下界、逆探索と前向きの論理Workを追加した。
queueは読み取るだけで、並び替えたりpopしたりしない。最良候補の費用・到着時刻は各Planの`search`に保持し、consoleには最大16検索の例を出す。

停止地点の`calls`は、その地点で止まったOD数。`observations`は同じOD内の繰返しを含む。複数地点のcallsやobservationsを足してOD数とは呼ばない。
`remaining_lower_bound <= best_complete_cost` は費用だけでなく同費用の改善を排除できない場合も含む。
Workのp95は1024単位の上端を用い、今回の製品問い合わせの上限は65,536。逆探索cache hitもcoldと同じ論理Workを再現する。

## 回帰と比較

Work 0〜65,536の全境界を旧pop-only方式と比較し、論理Work、Label、Memory、停止地点の一致を確認する。旧Certifiedの経路も一致を要求する。
完全Label生成直後、pop直前、目的地なし、より良いsuffixが未探索、同費用候補、Label/Memory admission、未知位相とNoRouteの区別を個別に確認する。

通常の65,536Workで実際に未popの完全経路を採用するfixtureは、64往復停車×80両の4サービスと直接サービス・接続サービスからなる。各サービスはDB公開の上限内に収める。
待機packetとイベント内cache、CargoPacket保存・復元、cache無効のpeerからの同一結果、遅延実車への予約を確認する。
既存の旅客回帰も通し、0/1/多段乗換、普通/特急と発車位相、車庫・繰返し停車・強制降車、マルチモード、Schedule-aware、Untimedを検証する。

実マップの品質比較は、別バイナリで1021問い合わせごとに同じOD・cargo・ready・除外Service・ライブ状態の旧方式と高Work参照を実行する。Provisionalは別の層として64件ごとにも採取する。
高Work参照はWork 1,048,576、Labels 65,536、Memory 128 MiB。参照がCertifiedの場合だけ最適費用・到着時刻との差に含める。
系統的な標本であり、全ODの品質保証とは扱わない。追加探索のCPU負荷があるため、このバイナリの時間は主性能比較に入れない。

## 実マップ条件と再現

入力Save SHA-256 `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`、config `bb9f97ff8504cbb287ef0976add7a33cb27440f5ecd9ccf4febf0aa4746fa122`、247資産の一覧 `339d61fef7c442e3d40f1007d48c2544484e38406e11263003cfce6238f97d42` を固定する。
参照元worktreeの削除により最初の試行はゲーム起動前に失敗した。ハッシュが一致するローカルファイルだけから、このworktree内へ独立コピーを復元した。失敗した試行を実測として採用しない。

MSVC x64 Release、`/O2 /Ob2 /MT`、assert ON、LTO OFF、null video/sound/music。
他のビルド・テスト・ゲームとの競合を共有ロックとプロセス監視で避け、起動前に90秒の静穏を確認する。検出された競合ランは速度比較から除外する。
Save・exe・NewGRFはGitへ保存せず、診断、console、ハッシュと検証結果を[測定資料](measurements/passenger-timed-anytime-2026-10-14/)へ保存する。

```powershell
# 依存関係は既存測定と同じ静的vcpkg一式を使う。
pwsh -NoProfile -File docs/measurements/passenger-timed-anytime-2026-10-14/Build-Variants.ps1 -Source . -Build build-timed-anytime -Dependencies PATH/TO/x64-windows-static
# build-timed-anytime/packageに、上記ハッシュを検証した資産・Save・configを置く。
python docs/measurements/passenger-timed-anytime-2026-10-14/Run-Sequence.py build-timed-anytime --output sequence2
python docs/measurements/passenger-timed-anytime-2026-10-14/Summarize-Anytime.py PATH/TO/MANIFEST.json --output PATH/TO/NEW-ARCHIVE
# 品質版: 再configure → Instrument-Profile.py --mode minimal → Prepare-Quality.py → openttd build。
python docs/measurements/passenger-timed-anytime-2026-10-14/Prepare-Quality.py . build-timed-anytime --stride 1021 --provisional-stride 64
python docs/measurements/passenger-timed-anytime-2026-10-14/Summarize-Quality.py PATH/TO/anytime-quality.jsonl --output PATH/TO/quality-summary.json
python docs/measurements/passenger-timed-anytime-2026-10-14/Verify-Final.py build-timed-anytime PATH/TO/ANYTIME-CHECKPOINT.sav PATH/TO/x64-windows-static
```

完了した測定グループを再実行せず続ける場合は、`Run-Sequence.py --start-at cold`等と、最初のAnytime結果の`--checkpoint`を指定する。出力directoryは新規にする。
途中でビルド競合を検出した対照ランも[除外記録](measurements/passenger-timed-anytime-2026-10-14/excluded-attempts.json)に残す。

## 回帰の実行結果

変更前の旅客回帰は351ケース・1,431,852アサーション、最初の変更後は357ケース・2,724,657アサーション、世代検証の追加後は **360ケース・2,725,303アサーション** が全件成功した。
全Work境界fixtureでは20境界で未pop候補を救済し、18境界では高Work参照より費用の高い候補を正しくProvisionalとして返した。生成直後2境界、pop直前6境界のWork終了も含む。
[変更後ログ](measurements/passenger-timed-anytime-2026-10-14/passenger-anytime-tests.log)には既存の300乗換や全国規模fixtureも含む。
測定資料の整合性テストには、FoundにProvisionalを混ぜない集計と、CMake OBJECT libraryのリンク先と差し替えTUのobject名が一致する回帰も含める。

## 世代検証追加前の幾何的な目的地Label診断

8,192 ticks、131,070検索の基準診断は、論理Work・実輸送・最終Saveが既存測定と一致した。最終Save SHA-256は `c8d4323412fdf2e85e53d30eda98d7815a639691a62f25f47b2cfeb3c8c7f2a9`。

| Work最終終了の検索 | 件数 |
|---|---:|
| 完全候補あり | 37,595 |
| 完全候補なし | 28,831 |
| 完全候補がqueueに残存 | 37,595 |
| pop済み完全候補あり | 0 |
| 最良候補の改善を排除できない下界が残存 | 37,595 |

この旧診断の「完全候補」は、世代検証前の目的地到達Labelを指す。保持する割合はWork終了の **56.60%**、全Timed検索の **28.68%**。現在の世代で実際に利用可能な完全チェーンの割合は、冒頭の最終original観測の47.79%で区別した。
Work終了検索で消費した逆探索Workは880,957,611、前向きWorkは3,472,336,725。各検索の両者の和は65,536。
最良候補の費用・到着時刻の例と停止地点の全カウンターは[基準診断](measurements/passenger-timed-anytime-2026-10-14/baseline/summary.json)に保存した。

この最初の診断版は、停止地点の下界を各Work消費時にも計算していたため、simulation平均42.873 msの観測負荷を含む。主性能比較には使わない。
製品では失敗したWork消費時だけ下界を評価する。速度対照は `Prepare-Legacy.py` による旧pop-only方式とし、Anytimeと同じ軽い診断・復元検証・`PlanWaiting()`を使う。完全チェーンの有効性検証を共有するため、この対照の実運転結果は、検証追加前のc2c4d1898cそのものとは区別する。

## 世代検証追加前のAnytime参考測定

8,192tickの2反復で、論理結果と最終Save SHA-256 `48cf100ffc5836b8637947e5add18bf5efbd76b262a90713cb9e961ee8453e68` が一致した。

| Timed検索・採用 | 件数 |
|---|---:|
| 検索 | 131,072 |
| Certified | 55,564 |
| Provisional | 4,779 |
| 未pop完全候補から新たに救済 | **4,778** |
| Work終了・完全候補あり | 4,744 |
| Work終了・完全候補なし | 42,000 |
| Provisionalのpacket採用 | **5,709** |
| Provisional採用の旅客数 | 6,192 |

これらは検索・採用の延べ件数であり、異なるODや旅客の一意数ではない。採用数にはイベント内cacheを使ったpacketも含む。
4,779件のProvisionalには、旧方式でもpop済み完全候補を返せた1件を含む。Work以外のLabel等の制限による救済も、4,778件の新規救済に含む。
変更前に観測した37,595件と変更後の4,778件は、経路採用で実運転と以後のOD・ready・Service状態が変わるため、同一の問い合わせ列ではない。同じライブ状態での差は別の品質probeで比較する。

対照作成の最初の試行では、改名したTUはコンパイルされたものの、CMakeのOBJECT libraryのconsumerが元のobject名を参照していたため、対照もAnytimeとして動いた。この結果を旧方式の速度対照には使わない。
差し替えTUは元のbasenameを保持し、モード間で該当する生成objectを除去して再コンパイルする。旧方式で`rescued_unpopped=0`、品質版で標本ファイルが生成されることも実行時に確認する。

## 世代検証追加前の品質参考標本

2,048tickで、1021検索ごとの系統標本33件と、64件ごとの追加Provisional標本24件を採取した。
最初の1024間隔は1tick当たり16検索の周期と重なり、Provisionalを採取できなかったため、こちらを品質評価の主資料にしない。

| 高Work参照との差 | 系統標本の利用可能20経路 | 追加Provisional 24経路 |
|---|---:|---:|
| 費用差 平均 | 0 | 40.75 |
| 費用差 中央値 / p95 / 最大 | 0 / 0 / 0 | 0 / 317 / 612 |
| 到着時刻差 平均 (tick) | 0 | 41.58 |
| 到着時刻差 中央値 / p95 / 最大 (tick) | 0 / 0 / 0 | 0 / 337 / 592 |

系統標本に含まれた1件と追加24件、合計25件のProvisionalはすべて旧pop-only方式では経路を返せず、高Work参照はCertifiedだった。
旧方式がCertifiedだった標本19件の費用・到着時刻の変化は0件。全57標本で同予算の旧方式と論理Workが一致した。
層を混ぜた平均を母集団の品質と解釈しない。結果と個別OD・ready・費用は[品質資料](measurements/passenger-timed-anytime-2026-10-14/quality-summary.json)と対応するjsonlに保存する。

継続運転の最初の試行は、既存NewGRFのcriticalな容量変更エラーでpauseし、8192 driver loopsに対して実際の進行は1669tickだった。この結果を8192tick継続として扱わない。
再試行は計測専用のコピーしたnullドライバで実StateTicksを数え、既存のbenchmark `unpause`でError pauseだけを解除した回数を記録する。製品のpause・車両容量・予約・乗車検証は変更しない。

これらは世代検証追加前の参考値。採用する最終値は本書冒頭の最終計測と `final-validated` にまとめた。
