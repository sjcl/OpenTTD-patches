# 統合後のJourneyPlanner 8192 tick基準値（604ce99389）

`jgrpp-ya` の `604ce993892479656a2c5ba2fbd311cb19c5d896` を測定した。通常車庫Wait修正、次発インデックス、Fallback binding再利用、Timed系統枝刈りを含む。製品経路処理は変更せず、consoleの翻訳単位コピーで既存ヒストグラムのp50とbucket出力だけを追加した最小計測版を使用した。

**3条件×2回の全6ランが有効。** 各回は同じ元セーブからStateTicks **3176545487 → 3176553679**、8192 simulation ticksを実行した。順序はoff/on/cold/cold/on/off。競合検出0、終了コード0、各ヒストグラム8192標本。以前の33.772 ms/tickは通常車庫Wait修正前かつ資産照合条件も異なるため、今回の改善率の基準には使わない。

測定中に先端へScratch再利用が追加統合され、`jgrpp-ya` は `c2c4d1898c` に進んだ。**この文書の45.39 ms/tickは604ce99389の結果であり、追加統合後の値ではない。**

追加統合後の再測定は[こちらの報告](passenger-routing-profile-c2c4d1898c-2026-10-13.md)を参照。

## 性能と反復

単位はms/tick。p50/p95/p99は1msヒストグラムの上端。最大値は記録されたtickの実測最大。反復の範囲は環境ノイズの観測で、信頼区間ではない。

| 条件 | 2回平均 | 各回平均 | p50（各回） | p95（各回） | p99（各回） | 最大（各回） | 平均の相対範囲 |
|---|---:|---|---|---|---|---|---:|
| Routing無効 | 21.997 | 21.929 / 22.065 | ≤22 / ≤22 | ≤28 / ≤28 | ≤33 / ≤34 | 55.513 / 60.942 | 0.616% |
| Routing有効・cache有効 | 45.390 | 45.390 / 45.390 | ≤45 / ≤45 | ≤65 / ≤65 | ≤85 / ≤85 | 141.985 / 124.343 | 0.00046% |
| Routing有効・派生cache無効 | 69.564 | 70.082 / 69.046 | ≤67 / ≤66 | ≤109 / ≤107 | ≤128 / ≤125 | 194.300 / 186.271 | 1.489% |

27 ms/tickまでの差は約18.39 ms/tick。キャッシュ有効のJourney部分は2回平均22.4095 ms/tick。Routing無効とのゲーム状態は異なるので、差分をそのままJourneyのexclusive CPU時間とは呼ばない。

## 時間内訳と優先順位

既存のsteady_clock観測によるsimulation threadの経過時間。OSのexclusive CPUサンプリング値ではない。以下は有効・最小計測版2回の平均で、上位項目を重複加算しない。

| 順位 | 範囲 | ms/tick | 次に確認する処理 |
|---:|---|---:|---|
| 1 | Timed前向き＋cacheその他 | 14.675 | Label・frontier・候補展開・queue。追加統合されたScratch再利用の効果を同じ8192 ticksで確認 |
| 2 | Timed逆向き下界のcold構築 | 4.658 | 共有OD policyのmiss/stale/eviction、下界再構築 |
| 3 | 残るJourney | 2.846 | Fallbackと待機・乗降の詳細プロファイル。残差を関数単体の時間とは扱わない |
| 4 | Schedule検索 | 0.169 | 主負荷に対して小さい |
| 5 | DB Tick（公開を含む） | 0.062 | 公開時間はこの内数で、別加算しない |

既存の前向き専用タイマーは約14.466 ms/tick。派生cache無効の前向きも約14.468 ms/tickであり、次発の実計算数の減少だけから前向きCPU時間の改善を主張しない。cache無効で大きく増えたのは逆向き下界とFallback再構築だった。待機、乗車、Fallback Distanceは包含時間なので上表へ足さない。全診断は[JSON集計](measurements/passenger-routing-profile-2026-10-13/merged-604ce99389/summary.json)、[順位JSON](measurements/passenger-routing-profile-2026-10-13/merged-604ce99389/optimization-ranking.json)に保存した。

## 呼出し・Work・結果一致

下記の件数は反復間で一致した。cacheによって変わる実行量と、保持すべき論理Workを分ける。

| 項目 | cache有効 | 派生cache無効 |
|---|---:|---:|
| Timed OD検索 | 131070 | 131070 |
| 論理車両候補 | 339158567 | 339158567 |
| 論理NextDeparture | 320806378 | 320806378 |
| 次発の実計算（索引内計算を含む） | 76097302 | 320806378 |
| Timed探索Work | 5037562007 | 5037562007 |
| policy hit / miss | 70342 / 60728 | 0 / 131070 |
| policy stale / eviction（共有LRU） | 8479 / 66503 | 0 / 0 |
| Fallback論理Work | 14837883320 | 14837883320 |
| Fallback実BFS検索 | 310404 | 452001 |
| Binding構築 / 全件検証nodes | 4 / 0 | 135263 / 0 |
| ReverseIndex構築 | 4 | 452001 |

Timed ODの最終理由はWork **66426**、Labels **309**、Memory **0**。重複観測はそれぞれ66832、323、0で、最終理由と合計しない。DB構築Work不足97、車両位相capture Work停止658、旧Graph構築Work停止0、Trip予測Work停止0。Fallback QueryWork延期は8936027 packetsで、再試行が重複する観測数である。詳細な探索段階別停止は別の詳細計測で取得する。

有効・派生cache無効の4ランで、最終Save SHA-256 **`c1d766e16ba9d9d3b6b3a97eca1c98353cf48b6bd253bf9bedd3eeb41629e01a`**、論理Work・理由と重複観測・実輸送が一致した。Timed実乗車/乗換/最終配送は43837/20724/12998 people、Untimedは558057/276375/229971、Scheduleは10/10/0。Scheduleの計画採用は44 packets/68 peopleで、実輸送とは別。Routing無効2回のSaveも互いに一致した。

## メモリと測定条件

| 条件 | peak Working Set bytes（各回） | sampled peak private bytes（各回） | sampled CPU seconds（各回） |
|---|---|---|---|
| off | 2020851712 / 2016677888 | 2022543360 / 2018148352 | 314.813 / 310.328 |
| on | 2083966976 / 2086744064 | 2085515264 / 2088833024 | 505.313 / 496.703 |
| cold | 2056568832 / 2062606336 | 2058469376 / 2064748544 | 695.344 / 688.281 |

プロセス観測は100ms間隔。CPU時間とメモリピークはロード・保存・workerを含み、tick区間のexclusive CPUや常駐RAMと混同しない。有効run末尾のDB見積もりは11987712 bytes、policy＋residency metadata見積もりは16642152 bytes、次発索引見積もりは402892 bytes・peak 419264 bytes。これらは製品診断の見積もりで、heapの実測値ではない。全件Binding検証は既定では実行されず、0 nodes / 0 nsだった。

CPUはAMD Ryzen 9 9950X。MSVC **19.44.35227.0** x64 Release、`/O2 /Ob2 /MT`、assert ON、LTO OFF、同じ静的vcpkgライブラリ。PowerShell 7.6.6。null video/sound/music。旅客CargoDist automatic、OD assignment/periodic有効、recalc_interval=8、recalc_time=32。offではRouting/periodicを無効化。

- 入力SHA-256: `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`。同じ16384×4096実マップの前回入力を使用。
- config SHA-256: `bb9f97ff8504cbb287ef0976add7a33cb27440f5ecd9ccf4febf0aa4746fa122`。
- 固定資産247 filesの一覧SHA-256: `339d61fef7c442e3d40f1007d48c2544484e38406e11263003cfce6238f97d42`。
- 最小計測版exe SHA-256: `5c133f3254408c1ff1e714bf7d237ee2afc533b2443334884749cc7b3ada8c25`。

前回の資産リンク先worktreeは削除済みで、今回固定したユーザー資産との完全一致は未確認。JP+ Multiple Unitsの容量変更と橋GRFのSpriteReplace警告が出た。全条件で同じ資産を使い、stderrのサイズ・原本SHA-256・重複除去した全警告行をJSONへ保存した。Save・NewGRF・exeはGitへ入れない。

## 再測定とAgent A/Bの追加

[Run-Profile.py](measurements/passenger-routing-profile-2026-10-13/Run-Profile.py)は既存の[測定ハーネス](measurements/passenger-periodic-policy-residency-2026-10-13/Measure-Residency.ps1)を使う。`--cache-off-scope all` は共有OD policy・departure index・Fallback resident bindings/reverse/distancesを無効化する。イベント内memoizationなどその他の製品動作は維持する。

```powershell
python docs/measurements/passenger-routing-profile-2026-10-13/Run-Profile.py run --package PATH/TO/FROZEN-PACKAGE --binary PATH/TO/openttd-minimal.exe --build PATH/TO/BUILD --output PATH/TO/FRESH-OUTPUT --label agent-a --revision FULL_COMMIT_SHA --cache-off-scope all --wait-idle-seconds 3600 --idle-grace-seconds 90
python docs/measurements/passenger-routing-profile-2026-10-13/Summarize-Profile.py PATH/TO/FRESH-OUTPUT/manifest.json --existing-summary docs/measurements/passenger-routing-profile-2026-10-13/merged-604ce99389/summary.json --output PATH/TO/comparison.json
```

既存実行を終了させずに待ち、各run前に90秒連続の競合なしを要求した。測定中は5秒ごとの監視。以前の競合ランは採用せず、今回の完走6ランとは別に保管する。監視間隔未満の競合を完全に検出する保証はない。

比較は入力・資産・config・コンパイラとライブラリ・CPU/ホスト・設定・区間・ハーネス・cache無効範囲が一致したminimal runだけで行う。詳細runは速度比較へ入れない。`--existing-summary` は収集時のSave検証を引き継ぎ、アーカイブにない大型Saveを再検証したとは扱わない。[Archive-Profile.py](measurements/passenger-routing-profile-2026-10-13/Archive-Profile.py)は原本とLF正規化済みconsoleの両ハッシュを記録して診断を保存する。

解析・比較・待機制御のPython検証は18件成功。604ce99389の詳細カウンターを有効にした旅客回帰は354 cases / 1347397 assertions成功。詳細時間はこの最小計測版の性能値へ混ぜていない。
