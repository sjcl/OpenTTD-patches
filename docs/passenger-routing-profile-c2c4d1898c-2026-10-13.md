# JourneyPlanner統合後の8192 tick再測定（c2c4d1898c）

測定対象は `jgrpp-ya` の **`c2c4d1898c3a38a091ff224c5be660ede72238f7`**。Scratch再利用を含む統合後の状態を固定し、製品ソースを変更せずに測定した。前回の33.772 ms/tickは通常車庫Wait修正前なので基準値に使わない。追加統合前の同条件測定は[604ce99389の報告](passenger-routing-profile-604ce99389-2026-10-13.md)を参照。

**主基準値はRouting無効17.244、cache有効32.347、派生cache無効52.115 ms/tick。** 最小計測版で3条件×2回を追加測定し、反復間の相対範囲は0.070% / 1.623% / 1.451%だった。cache有効は27 ms/tickを約5.35 ms/tick上回る。

先行の3条件×2回には20～23%の環境変動があった。この6ランを隠さず保存し、独立した詳細計測の後に、同じ最小計測バイナリでもう一巡測定した。先行6＋追加6＋詳細1の**全13ランが有効**で、競合検出0、終了コード0、各8192 ticks。詳細計測の実行時間は速度比較に入れない。

診断と生consoleは[JSON集計](measurements/passenger-routing-profile-2026-10-13/merged-c2c4d1898c/summary.json)、[順位JSON](measurements/passenger-routing-profile-2026-10-13/merged-c2c4d1898c/optimization-ranking.json)、[収集index](measurements/passenger-routing-profile-2026-10-13/merged-c2c4d1898c/collection-index.json)へ保存した。

## 最終基準値（追加6ラン）

集計labelは`current-repeat`。同じ元セーブと8192 ticks全体を使い、順序はoff/on/cold/cold/on/off。平均は2回のrun平均の平均。p50/p95/p99はnearest-rank法による1msヒストグラムの上端、最大は実測値。相対範囲は環境ノイズの観測で、信頼区間ではない。

| 条件 | 2回平均 | 各回平均 | p50（各回） | p95（各回） | p99（各回） | 最大（各回） | 平均の相対範囲 |
|---|---:|---|---|---|---|---|---:|
| Routing無効 | 17.244 | 17.238 / 17.250 | ≤17 / ≤17 | ≤22 / ≤22 | ≤26 / ≤25 | 43.515 / 39.004 | 0.070% |
| Routing有効・cache有効 | 32.347 | 32.610 / 32.085 | ≤32 / ≤31 | ≤48 / ≤46 | ≤65 / ≤64 | 99.699 / 98.833 | 1.623% |
| Routing有効・派生cache無効 | 52.115 | 52.493 / 51.737 | ≤50 / ≤49 | ≤83 / ≤81 | ≤97 / ≤94 | 121.498 / 116.949 | 1.451% |

cache有効のJourney平均は13.8145 ms/tick、Journeyのp95は≤28/≤27、p99は≤46/≤45、最大79.812/81.733 ms。Routing無効とのゲーム状態は異なるので、両者の全体時間の差をJourneyのexclusive CPU時間とは呼ばない。

## 主測定の処理カテゴリ

最小計測版の追加2回平均、ms/tick。包含関係がある値を合計しない。

| 処理 | cache有効 | 派生cache無効 |
|---|---:|---:|
| Timed OD query（包含） | 11.318581 | 22.837873 |
| 逆向き下界cold | 3.912754 | 15.158200 |
| 前向き探索 | 7.241828 | 7.344682 |
| 前向き＋cacheその他 | 7.405827 | 7.679673 |
| 次発index構築 | 0.003168 | 0.000000 |
| Schedule検索（包含） | 0.134391 | 0.625955 |
| DB Tick（公開を含む） | 0.052510 | 0.051552 |
| DB公開（内数） | 0.019695 | 0.018570 |
| Fallback Distance（包含） | 1.843214 | 9.465222 |
| Binding再構築 | 0.000021 | 0.934006 |
| Binding全件検証 | 0.000000 | 0.000000 |
| ReverseIndex構築 | 0.000044 | 6.059264 |
| 待機処理（包含） | 11.801083 | 23.804297 |
| 乗車処理（包含） | 1.568721 | 7.679618 |

次発の実計算数は約4.2倍違うが、前向き時間は両条件で近い。呼出し数だけから次発計算のCPU改善量を推測しない。coldで大きく増えるのは逆向き下界とFallback再構築である。

## 条件と証拠

すべて同じ16384×4096実マップの元セーブから、StateTicks **3176545487 → 3176553679**、**8192 simulation ticks**を実行した。最小計測版はconsole翻訳単位のコピーに既存1msヒストグラムのp50とbucket出力を追加しただけで、hot loopへ追加タイマーやカウンターを置かない。詳細版は独立した7翻訳単位のコピーに観測を追加する。

| 固定対象 | 値 |
|---|---|
| 入力Save SHA-256 | `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390` |
| config SHA-256 | `bb9f97ff8504cbb287ef0976add7a33cb27440f5ecd9ccf4febf0aa4746fa122` |
| 資産247 filesの一覧SHA-256 | `339d61fef7c442e3d40f1007d48c2544484e38406e11263003cfce6238f97d42` |
| 最小計測版exe SHA-256 | `a525fcc74c441f63a7dc456d1f9ee2ea84342be1f2db514d75ea13d4903db512` |
| 詳細版exe SHA-256 | `585086b99d17cb642eca0e897f075197721a20dc3f5f38ff37c7fb6a6b1ec846` |
| 詳細観測header SHA-256 | `639d144f28e4e2f7f4e5625eb3544e948ceed53f398a8bebcd596b6c3d309aee` |

CPUはAMD Ryzen 9 9950X、16 cores / 32 logical processors。MSVC **19.44.35227.0** x64 Release、`/O2 /Ob2 /MT`、assert ON、LTO OFF、同じ静的vcpkgライブラリを使用した。PowerShell 7.6.6、null video/sound/music。旅客CargoDist automatic、OD assignment/periodic有効、recalc_interval=8、recalc_time=32。offではRouting/periodicを無効化。CPU affinityは設定しない。

`--cache-off-scope all` のcold条件は、Timed/Schedule共有OD policy、次発インデックス、Fallback resident binding/reverse/distance cacheを無効化する。イベント内memoizationと、今回統合された探索Scratch再利用は維持する。すべての一時記憶を無効化する条件ではない。

入力Saveは前回SHAと一致した。一方、前回の資産リンク先worktreeは削除済みで、以前の資産全体との完全一致は未確認。604ce99389と今回の比較には同じ固定パッケージを使い、今回生成したbaseset/langも固定パッケージとの一致を確認した。JP+系の車両容量変更と橋GRFのSpriteReplace警告が出る。各stderrの原本SHA・サイズ・重複除去した全行をJSONに保存する。Save/NewGRF/exeはGitへ入れない。

既存の他チャットの実行は終了を待つ。共有計測ロックと各run前の90秒連続の競合なしを使い、測定中は5秒ごとにビルド・テスト・ゲームの競合を監視する。競合開始時は自分の測定だけを中断し、そのランを採用しない。5秒未満の競合を完全に検出する保証はない。

## 先行6ランと環境変動

単位はms/tick。p50/p95/p99は1msヒストグラムの上端、最大は実測値。各条件とも終了コード0、競合検出0、8192標本、反復間の保存データと論理結果は一致した。

| 条件 | 2回平均 | 各回平均 | p50（各回） | p95（各回） | p99（各回） | 最大（各回） | 平均の相対範囲 |
|---|---:|---|---|---|---|---|---:|
| Routing無効 | 19.433 | 21.690 / 17.176 | ≤22 / ≤17 | ≤28 / ≤22 | ≤32 / ≤25 | 62.583 / 44.301 | 23.233% |
| Routing有効・cache有効 | 36.660 | 40.405 / 32.914 | ≤39 / ≤32 | ≤59 / ≤48 | ≤81 / ≤67 | 126.849 / 102.945 | 20.435% |
| Routing有効・派生cache無効 | 58.280 | 64.294 / 52.266 | ≤61 / ≤50 | ≤100 / ≤81 | ≤116 / ≤92 | 144.831 / 119.349 | 20.637% |

すべて同じexeと入力で、順序はoff/on/cold/cold/on/off。時系列後半で、探索以外の処理とプロセスCPU時間も短くなった。原因は未確認。測定中に確認した電源設定は「バランス」だったが、各先行runのCPU clock・電源状態は収集していないので、原因や一定のCPU環境を断定しない。

比較スクリプトの`compatible`は入力・資産・config・compiler/library・CPU/host・設定・区間・harness・cache範囲の一致を表す。CPU環境の時間的安定を証明する値ではない。`relative_change`を変更単独の効果と断定しない。

## 論理Work、実計算、キャッシュ、輸送

以下の件数は先行・追加反復間で一致し、詳細版も同じ論理結果を保った。

| 項目 | cache有効 | 派生cache無効 |
|---|---:|---:|
| Timed OD検索 | 131070 | 131070 |
| 論理車両候補 | 339158567 | 339158567 |
| 論理NextDeparture | 320806378 | 320806378 |
| 次発の実計算（索引内の計算を含む） | 76097302 | 320806378 |
| Timed探索Work | 5037562007 | 5037562007 |
| 受理Label / frontier lookup | 92489788 / 1586055611 | 同じ |
| queue push / pop | 92489788 / 6980559 | 同じ |
| Timed policy hit / miss | 70342 / 60728 | 0 / 131070 |
| policy stale / eviction（共有LRU） | 8479 / 66503 | 0 / 0 |
| Schedule OD / hit / miss | 80752 / 65561 / 15191 | 80752 / 0 / 80752 |
| Schedule論理Work | 135822634 | 135822634 |
| Fallback論理Work | 14837883320 | 14837883320 |
| Fallback実BFS | 310404 | 452001 |
| Binding構築 / 全件検証nodes | 4 / 0 | 135263 / 0 |
| ReverseIndex構築 | 4 | 452001 |

Timed最終延期理由はWork **66426**、Labels **309**、Memory **0**。重複観測は66832、323、0で、最終理由と足さない。DB構築Work不足97、車両位相capture Work停止658、Graph/Trip Work停止0。Fallback QueryWork延期は8936027 packetsで、再試行を含む観測数。

詳細版の前向きWork停止観測は172271回。`spend()`の停止地点はqueue 66731、alighting 58968、dominance 35060、vehicle phase 5269、candidate service 235、dispatch slot 0。合計166263。測定対象の`PlanInternal`にあるWork停止は`spend()`失敗と次発評価直後の上限判定の2経路だけなので、残る6008回は後者に対応する差分である。これは直接の地点カウンターではなく、観測合計とソース経路の照合で求めた。前向きLabel停止観測2439、Memory停止0。これらは繰返し観測なので、66426/309というODの最終理由件数と一致する必要はない。停止位置の内訳であり、各位置で消費したWork量の配賦ではない。

先行＋追加on/cold 8ランと詳細on 1ランの最終Save SHA-256は **`c8d4323412fdf2e85e53d30eda98d7815a639691a62f25f47b2cfeb3c8c7f2a9`**。論理Work・理由/重複観測・実輸送も一致した。Timed実乗車/乗換/最終配送は43837/20724/12998 people、Untimedは558057/276375/229971、Scheduleは10/10/0。Schedule計画採用44 packets/68 peopleは実輸送と区別する。

604ce99389と論理診断の比較も一致するが、コミット間のSave全体SHAは異なる。原因は調べておらず、コミット間の全ゲーム状態一致は主張しない。キャッシュ有無の一致は同一コミット内の全Save SHAで確認する。

## 独立した詳細プロファイル

以下はcache有効・8192 ticks・1ランの観測。タイマーとカウンターの負荷を含むので、性能比較の数値として使わない。時間はsimulation threadのsteady_clock経過時間。`exclusive_ns`は観測した子scopeだけを差し引いた経過時間で、OSのexclusive CPUサンプルではない。

| 処理scope | 呼出し数 | 包含 ms/tick | 観測した子を除く ms/tick |
|---|---:|---:|---:|
| Timed Plan | 131070 | 14.460389 | 0.187464 |
| 逆向き下界cold構築 | 60728 | 4.198394 | 4.198394 |
| 前向き探索 | 117966 | 10.074531 | 7.425852 |
| 候補の次発評価 | 52297619 | 2.475393 | 0.410632 |
| 次発indexアクセス | 52291611 | 0.401024 | 0.397800 |
| index lookup | 52291611 | 1.661478 | 0.397018 |
| indexed successor | 52291611 | 1.260064 | 0.383007 |
| index tree Find（再帰を含む） | 202916312 | 2.098882 | 0.877057 |
| index構築 | 12918 | 0.003223 | 0.003223 |
| 明示的NextDeparture関数 | 1779268 | 0.004489 | 0.004489 |
| 非索引の車両位相scan | 6008 | 0.002259 | 0.002167 |
| Schedule Plan | 80752 | 0.159167 | 0.015863 |
| Schedule Search | 15191 | 0.143303 | 0.141932 |
| Schedule待機推定 | 17480 | 0.001371 | 0.001371 |
| Fallback Distance | 8290708 | 2.132777 | 0.402583 |
| Fallback BFS loop | 310404 | 1.719183 | 1.719183 |
| Node bindingアクセス | 135263 | 0.005099 | 0.005065 |
| Node binding cold再構築 | 4 | 0.000033 | 0.000033 |
| ReverseIndex構築 | 4 | 0.000059 | 0.000059 |
| resident distance cache再利用 | 141597 | 0.005854 | 0.005854 |
| 待機CargoPacket走査 | 179753 | 15.005134 | 0.377802 |
| Intent current検証 | 1634037 | 0.010808 | 0.010808 |
| Intent＋実ride検証 | 1279568 | 0.024022 | 0.010253 |
| 実車両ride検証 | 75925 | 0.007741 | 0.003994 |
| 車両Station施設検証 | 12353936 | 0.066811 | 0.066811 |
| CargoPacket Load | 566351 | 2.697715 | 0.910510 |
| CargoPacket Stage | 90258 | 0.539874 | 0.111837 |
| CargoPacket Append | 730656 | 0.057900 | 0.057900 |
| 待機packet再分類・再挿入 | 179753 | 0.002379 | 0.002379 |

親子の包含時間を足さない。特に再帰的Findは自身を包含し、Load/StageにはFallback等が含まれる。Load/Stage/Appendは旅客以外のcargo処理も通る汎用関数である。明示的NextDeparture関数1779268回は、索引内の算術を含む製品診断76097302回とは異なる範囲。

追加カウンターは、候補系統展開57123211、Label試行1586055611、支配判定2062786383、待機packet訪問15292891、再挿入packet 256808。非索引のphase訪問43980、index構築のphase訪問76590、indexのlive loading vehicle訪問47967119。Live Station観測はGet 2035、GetIfValid 97828451、IsValidID 159065935、Timed ODの明示的Station検証262140。Live Station総数はコピーした経路関連翻訳単位内の参照で、ゲーム全体やTimedだけの件数ではない。

Node binding全件検証は既定で無効なので0 nodes / 0 ns。未実行の検証loopの性能を推測しない。詳細版はreset後のepoch付き参照を使い、各カウンターで文字列辞書を毎回検索する負荷を避けた。カウンター参照のresetとscopeの親子時間を専用の旅客テストで確認した。

## 初期区間と後続区間

詳細版の8192 tick生標本と、1024 tickごとの累積scope差分をJSONに保存する。以下は詳細版の包含時間なので、最小計測版の速度比較には使わない。

| tick序数 | Simulation平均 ms | Journey平均 ms | DB Tick ms/tick | 公開回数 | 逆向きcold ms/tick | 前向き ms/tick |
|---|---:|---:|---:|---:|---:|---:|
| 1–1024 | 44.146 | 18.799 | 0.144 | 3365 | 5.030 | 10.418 |
| 1025–2048 | 39.358 | 17.678 | 0.078 | 12 | 4.077 | 10.742 |
| 2049–3072 | 35.712 | 16.584 | 0.063 | 12 | 4.240 | 9.699 |
| 3073–4096 | 37.588 | 17.442 | 0.085 | 15 | 4.506 | 10.052 |
| 4097–5120 | 35.377 | 16.500 | 0.060 | 12 | 3.914 | 9.657 |
| 5121–6144 | 35.218 | 16.551 | 0.033 | 6 | 3.731 | 10.017 |
| 6145–7168 | 37.027 | 17.609 | 0.072 | 12 | 4.148 | 10.345 |
| 7169–8192 | 35.441 | 16.589 | 0.029 | 6 | 3.942 | 9.667 |

初期1024 ticksに公開3365回が集中する。その後も公開とcold下界構築は続くので、1025 tick以降を完全な定常状態とは宣言しない。最小計測版の主条件は元セーブから8192 ticks全体のままで、異なるシミュレーション区間へ置き換えない。

## メモリとプロセスCPU

| 条件 | peak Working Set bytes（各回） | sampled peak private bytes（各回） | sampled CPU seconds（各回） |
|---|---|---|---|
| off | 2015592448 / 2014576640 | 2019127296 / 2018066432 | 238.016 / 241.703 |
| on | 2083942400 / 2084130816 | 2087288832 / 2087751680 | 363.391 / 361.172 |
| cold | 2055979008 / 2053763072 | 2059784192 / 2057003008 | 523.641 / 522.531 |

プロセス観測は100ms間隔。CPU時間とメモリピークはロード・保存・workerを含み、tick区間のexclusive CPUや常駐RAMではない。cache有効のDB見積もり11987712 bytes、policy＋residency metadata見積もり16642152 bytes、Fallback cache診断16871860 bytes、次発index見積もり402892 bytes（peak 419264）、探索Scratch保持見積もり849430 bytes。coldのpolicy＋metadataは917496、次発indexは0、探索Scratchは849406 bytes。製品診断の見積もりはheap実測値と区別する。

## 再測定とAgent A/B比較

[Build-Profile.ps1](measurements/passenger-routing-profile-2026-10-13/Build-Profile.ps1)で各コミットの最小計測版を作り、[Run-Profile.py](measurements/passenger-routing-profile-2026-10-13/Run-Profile.py)で同じ固定パッケージを再生する。既存の[Measure-Residency.ps1](measurements/passenger-periodic-policy-residency-2026-10-13/Measure-Residency.ps1)を再利用する。詳細版は`-Mode detail`で別exeへsnapshotし、`--kind detail --modes on --repetitions 1`で独立した出力先へ測定する。比較対象のビルドとテストは測定前に終える。

```powershell
pwsh -NoProfile -File docs/measurements/passenger-routing-profile-2026-10-13/Build-Profile.ps1 -Source PATH/TO/CHECKOUT -Build PATH/TO/BUILD -Dependencies PATH/TO/VCPKG/x64-windows-static -Mode minimal
python docs/measurements/passenger-routing-profile-2026-10-13/Run-Profile.py run --package PATH/TO/FROZEN-PACKAGE --binary PATH/TO/BUILD/openttd-minimal.exe --build PATH/TO/BUILD --output PATH/TO/FRESH-OUTPUT --label agent-a --revision FULL_COMMIT_SHA --cache-off-scope all --wait-idle-seconds 3600 --idle-grace-seconds 90
python docs/measurements/passenger-routing-profile-2026-10-13/Summarize-Profile.py PATH/TO/FRESH-OUTPUT/manifest.json --existing-summary docs/measurements/passenger-routing-profile-2026-10-13/merged-c2c4d1898c/summary.json --baseline current-repeat --output PATH/TO/comparison.json
python docs/measurements/passenger-routing-profile-2026-10-13/Rank-Profile.py PATH/TO/comparison.json --output PATH/TO/ranking.json
```

Agent B等も異なるlabelと新規outputを使い、集計コマンドに追加のmanifestを渡す。比較は同じ区間のminimal runだけを使う。詳細版のタイマーは経路選択・RNG・Work・cache・保存形式を変更しない観測だが、処理時間を増やす。詳細版の時間を主性能値へ混ぜない。

集計JSONには実計算と論理Work、反復の相対範囲、キャッシュ有無の全Save一致、コミット間の論理結果一致と全Save一致をそれぞれ保存する。`--existing-summary`は収集時の保存データ検証を引き継ぎ、アーカイブに入っていない大型Saveを再検証したとは扱わない。[Archive-Profile.py](measurements/passenger-routing-profile-2026-10-13/Archive-Profile.py)は診断・生console・原本とLF正規化後のハッシュを保存する。

詳細カウンターを有効にしたC++旅客回帰は、隠し性能ベンチを除く`[passenger]~[.benchmark]`で **351 cases / 1429274 assertions** 成功した。生成された観測専用テストでcounter参照のreset、disabled状態、親scopeの時間＝自身＋子scopeの時間、scope stackの解除を検証した。製品ソースの変更はない。

解析・比較・待機制御のPython検証は **23件成功**。[検証metadata](measurements/passenger-routing-profile-2026-10-13/merged-c2c4d1898c/verification-metadata.json)にログの原本とLF正規化後のSHAを保存した。最初の詳細起動は他チャットの計測ロックによりゲーム起動前に終了し、既存実行の完走後に再実行した。計測時のrunnerは証拠snapshotを保存し、提供するrunnerにはロック解放を待ってから大型資産を検証する制御とidle条件の記録を追加した。

## 次の最適化対象の順位

最小計測版cache有効2回の、重複しない上位scopeの経過時間を根拠にした順位。OS CPUサンプルの割合ではない。詳細版は調査箇所を絞る補助に使う。

| 順位 | 範囲 | ms/tick | 対象 |
|---:|---|---:|---|
| 1 | Timed前向き＋cacheその他 | 7.406 | frontier lookup・Label試行・支配判定を最優先に調査。次発index lookupとlive loading vehicle走査を次に確認 |
| 2 | 逆向き下界cold構築 | 3.913 | policy miss/stale/共有LRU evictionによる再構築を減らす |
| 3 | 残るJourney | 2.309 | Fallback BFSを優先。待機・移動の子scopeを二重加算しない |
| 4 | Schedule検索 | 0.134 | 待機推定は小さく、主負荷の最適化後に確認 |
| 5 | DB Tick | 0.053 | 初期公開を含め小さい。公開はこの内数 |

前向き本体は詳細版で、観測した子scopeを除いて7.426 ms/tickだった。Label試行15.86億回・支配判定20.63億回を観測したが、個々のLabel/支配関数だけのCPU時間は分離していない。BFS loopは詳細版1.719 ms/tick、resident Binding/ReverseIndex再構築は小さかった。待機走査の15.005 ms/tickには検索が含まれ、観測した子を除く値は0.378 ms/tickなので、待機scanの包含時間だけを根拠に最優先とはしない。

27 ms/tickまで約5.35 ms/tickの短縮が必要。上位の前向き処理とcold下界を先に調査し、既存の論理Work・Label/Memory延期・経路結果を保つ変更を同じ入力で検証する。604ce99389との[比較JSON](measurements/passenger-routing-profile-2026-10-13/merged-c2c4d1898c/comparison-604ce99389.json)も保存したが、Routing無効の時間も変化したため、観測差をコード変更単独の改善率とは扱わない。
