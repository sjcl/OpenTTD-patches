# f97bac71 / 6c11c67f Clang ThinLTO ABBA計測

A=`f97bac717c840852d8240f0eac51a92bad0abb9b`、B=`6c11c67fd08115ce370fbfb182cdc952f37d75fd`。同じ凍結済み`gc3833d807.sav`から、Legacy／HyperpathをそれぞれA→B→B→Aで各2,048 tick採取した（2026-10-19夜開始、JST）。
**採用8本すべて、終了canonical 24項目が前回f97bac71の同じモードおよび今回の最初のAと完全一致した。**
両版ともClangCL / LLD 23.1.3、ThinLTO、assert OFF、PGO OFF。同じコンパイラ・ビルド条件で製品変更を比較している。

時間は主にメインスレッドの`StateGameLoop`を含むSampledProfileのCPU ms/tick。平均は各版・各モード2本、範囲は実測最小～最大。samplesは2本合計。
`StepImpl self`はスタック末端が`PassengerPrecomputedTable::StepImpl`であるサンプルのみ。ほかの指定関数・経路はinclusiveで、同じスタックを1回だけ数える。親子のinclusiveを加算していない。

## 観測した差

Hyperpathの`StepImpl` selfは0.261657→0.012971 ms/tick（−95.04%）、inclusiveも0.406058→0.115625（−71.52%）へ減った。
ワーカーの`PrepareHyperpathFinish`は新たに0.278744 ms/tickあり、行の仕上げがメインからワーカーへ移ったという実装と整合する。
`RetiredPlan`は0.117426→0.080056（−31.82%）、その配下の`PlanPrecomputed`は0.093263→0.029286（−68.60%）へ減った。

一方、**`HyperpathLookupCandidates`→`PlanPrecomputed`は0.022467→0.021752（−3.18%）、92→89 samplesにとどまり、期待した短縮は今回確認できない。**
lookup全体のinclusiveは0.135028→0.153601（+13.75%）へ増えた。新しい`SharedHyperpathPlan`はinclusive 0.115740、self 0.054450 ms/tickを占める。
共有処理のどこが支配的かは、この関数集計だけでは断定しない。`PlanPrecomputed`全体では0.301379→0.213626（−29.12%）だが、これをlookup配下の改善と混同しない。

Hyperpathのsimulation CPU平均は23.265649→23.026597（−1.03%）、tick実時間は24.830950→24.175775（−2.64%）。
ただしCPUの実測範囲はA 22.340818～24.190479、B 22.513335～23.539860と重なり、LegacyのCPU平均も−1.19%だった。
**局所的な削減は確認できるが、tick全体の約1%を再現性のある高速化とは断定しない。**
Legacyの`RetiredPlan`は0.408696→0.437507（+7.05%）で、2つ目の共有による短縮は観測していない。

## Hyperpath

| 対象 | A 平均 [範囲] | B 平均 [範囲] | B−A | 変化 | samples A/B |
| --- | ---: | ---: | ---: | ---: | ---: |
| `StateGameLoop` | 23.265649 [22.340818, 24.190479] | 23.026597 [22.513335, 23.539860] | -0.239051 | -1.03% | 95282 / 94296 |
| `StepImpl self` | 0.261657 [0.245986, 0.277329] | 0.012971 [0.012241, 0.013701] | -0.248686 | -95.04% | 1070 / 53 |
| `StepImpl inclusive` | 0.406058 [0.393749, 0.418367] | 0.115625 [0.105407, 0.125844] | -0.290433 | -71.52% | 1660 / 472 |
| `RetiredPlan` | 0.117426 [0.116208, 0.118643] | 0.080056 [0.077124, 0.082988] | -0.037370 | -31.82% | 481 / 328 |
| `RetiredPlan -> PlanPrecomputed` | 0.093263 [0.089359, 0.097167] | 0.029286 [0.023908, 0.034664] | -0.063977 | -68.60% | 382 / 120 |
| `HyperpathLookupCandidates` | 0.135028 [0.132319, 0.137738] | 0.153601 [0.152918, 0.154284] | +0.018572 | +13.75% | 553 / 629 |
| `HyperpathLookupCandidates -> PlanPrecomputed` | 0.022467 [0.021491, 0.023442] | 0.021752 [0.020067, 0.023437] | -0.000714 | -3.18% | 92 / 89 |
| `SharedHyperpathPlan` | 0.000000 [0.000000, 0.000000] | 0.115740 [0.108941, 0.122538] | +0.115740 | — | 0 / 474 |
| `SharedHyperpathPlan self` | 0.000000 [0.000000, 0.000000] | 0.054450 [0.054226, 0.054674] | +0.054450 | — | 0 / 223 |
| `PlanPrecomputed` | 0.301379 [0.298467, 0.304290] | 0.213626 [0.203434, 0.223818] | -0.087753 | -29.12% | 1234 / 874 |
| `TickPrecomputed` | 0.416397 [0.402560, 0.430234] | 0.126856 [0.117609, 0.136103] | -0.289541 | -69.53% | 1702 / 518 |
| `simulation_wall_ms_per_tick` | 24.830950 [24.230779, 25.431122] | 24.175775 [23.561984, 24.789565] | -0.655176 | -2.64% | — / — |

## Legacy

| 対象 | A 平均 [範囲] | B 平均 [範囲] | B−A | 変化 | samples A/B |
| --- | ---: | ---: | ---: | ---: | ---: |
| `StateGameLoop` | 23.765567 [22.941898, 24.589236] | 23.483554 [23.270487, 23.696620] | -0.282013 | -1.19% | 97334 / 96176 |
| `StepImpl self` | 0.101265 [0.100761, 0.101769] | 0.096429 [0.092994, 0.099864] | -0.004835 | -4.78% | 414 / 394 |
| `StepImpl inclusive` | 0.168788 [0.165850, 0.171725] | 0.167660 [0.161012, 0.174308] | -0.001128 | -0.67% | 690 / 685 |
| `RetiredPlan` | 0.408696 [0.386238, 0.431153] | 0.437507 [0.436515, 0.438498] | +0.028811 | +7.05% | 1674 / 1792 |
| `RetiredPlan -> PlanPrecomputed` | 0.328136 [0.310558, 0.345715] | 0.341567 [0.340849, 0.342285] | +0.013430 | +4.09% | 1344 / 1399 |
| `HyperpathLookupCandidates` | 0.101812 [0.097669, 0.105955] | 0.093045 [0.088389, 0.097702] | -0.008767 | -8.61% | 417 / 381 |
| `HyperpathLookupCandidates -> PlanPrecomputed` | 0.000000 [0.000000, 0.000000] | 0.000000 [0.000000, 0.000000] | +0.000000 | — | 0 / 0 |
| `SharedHyperpathPlan` | 0.000000 [0.000000, 0.000000] | 0.004637 [0.004394, 0.004881] | +0.004637 | — | 0 / 19 |
| `SharedHyperpathPlan self` | 0.000000 [0.000000, 0.000000] | 0.004637 [0.004394, 0.004881] | +0.004637 | — | 0 / 19 |
| `PlanPrecomputed` | 0.488691 [0.460074, 0.517308] | 0.486485 [0.483554, 0.489415] | -0.002206 | -0.45% | 2001 / 1992 |
| `TickPrecomputed` | 0.173687 [0.171744, 0.175630] | 0.173555 [0.167361, 0.179748] | -0.000132 | -0.08% | 710 / 709 |
| `simulation_wall_ms_per_tick` | 24.707143 [24.109586, 25.304699] | 24.360436 [24.290206, 24.430667] | -0.346707 | -1.40% | — / — |

Legacyではtick間のHyperpathプラン共有が適用されない。`SharedHyperpathPlan`がnullptrを返す判定コストはあり得る。関数別の小差だけを共有による改善と解釈しない。

## 各本の生値とばらつき

| 順序 | 版 | モード | simulation CPU ms/tick | tick実時間 ms/tick | StepImpl self | RetiredPlan inclusive | lookup→plan inclusive |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 01-A1/legacy | A | legacy | 22.941898 | 24.109586 | 0.100761 | 0.386238 | 0.000000 |
| 01-A1/hyperpath | A | hyperpath | 24.190479 | 25.431122 | 0.277329 | 0.116208 | 0.021491 |
| 02-B1/legacy | B | legacy | 23.270487 | 24.290206 | 0.099864 | 0.438498 | 0.000000 |
| 02-B1/hyperpath | B | hyperpath | 22.513335 | 23.561984 | 0.012241 | 0.077124 | 0.020067 |
| 03-B2/legacy | B | legacy | 23.696620 | 24.430667 | 0.092994 | 0.436515 | 0.000000 |
| 03-B2/hyperpath | B | hyperpath | 23.539860 | 24.789565 | 0.013701 | 0.082988 | 0.023437 |
| 04-A2/legacy | A | legacy | 24.589236 | 25.304699 | 0.101769 | 0.431153 | 0.000000 |
| 04-A2/hyperpath | A | hyperpath | 22.340818 | 24.230779 | 0.245986 | 0.118643 | 0.023442 |

各ラウンド内はLegacy→Hyperpath、ラウンドはA1→B1→B2→A2。すべて新規プロセス・同じ固定入力で開始した。
B2 Legacyの最初の試行は別チャットのninja/lld-linkと重なり、runnerと区間監査が不合格にしたため除外した。採用済み4本は維持し、その1本だけ新規ゲートで取り直して残りを継続した。rejection.jsonとchecks/rejectedに不採用理由を保存した。
ABBAで時間に対して直線的なドリフトを釣り合わせるが、全てのノイズを除去するものではない。2回ずつの範囲は信頼区間ではない。untouchedによる追加補正は行っていない。
両版ともWPAで`StepHyperpath`の独立したフレームは観測しておらず、指定された`StepImpl`の同じself境界を比較した。0サンプルの経路は処理時間0の証明ではない。

## ワーカーと診断値

parallel rowsは終了時の現行generationの累積値であり、2,048 tick区間の差分カウンターではない。

`PrepareHyperpathFinish`はワーカー側を別集計した。これはメインスレッドのinclusiveと足していない。ワーカーへの処理移動によるメインCPU削減と、全スレッドCPU削減を区別する。

| 対象 | A 平均 [範囲] | B 平均 [範囲] | B−A | 変化 | samples A/B |
| --- | ---: | ---: | ---: | ---: | ---: |
| `worker PrepareHyperpathFinish` | 0.000000 [0.000000, 0.000000] | 0.278744 [0.276915, 0.280573] | +0.278744 | — | 0 / 1141 |
| `all_threads_cpu_ms_per_tick` | 27.270147 [26.667709, 27.872585] | 26.953971 [26.209073, 27.698869] | -0.316176 | -1.16% | — / — |

| 実行 | parallel rows（cargoごと） | lookup rebuilds | indexed_groups |
| --- | --- | ---: | ---: |
| 01-A1/legacy | 旧版はこの行なし | 2,709 | 195,099 |
| 01-A1/hyperpath | 旧版はこの行なし | 2,714 | 203,870 |
| 02-B1/legacy | cargo=0; workers=3; cache_hits=113; serial_rows=1; finished_rows=0; finish_effects=0; throttles=0 | 2,709 | 195,099 |
| 02-B1/hyperpath | cargo=0; workers=4; cache_hits=333; serial_rows=1; finished_rows=332; finish_effects=5278409; throttles=0 | 2,714 | 203,870 |
| 03-B2/legacy | cargo=0; workers=3; cache_hits=113; serial_rows=1; finished_rows=0; finish_effects=0; throttles=0 | 2,709 | 195,099 |
| 03-B2/hyperpath | cargo=0; workers=4; cache_hits=333; serial_rows=1; finished_rows=332; finish_effects=5278409; throttles=0 | 2,714 | 203,870 |
| 04-A2/legacy | 旧版はこの行なし | 2,709 | 195,099 |
| 04-A2/hyperpath | 旧版はこの行なし | 2,714 | 203,870 |

## 条件・品質・保管

- 固定入力46,620,248 bytes、SHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットも前回manifestに照合。StateTicks **3,197,276,448 → 3,197,278,496**。
- `--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。replay・warm・背景処理が完了し、準備中tick不変。追加warm-up tick・cold・再構築指定なし。
- `openttd_lib`／exe／testの実コンパイルコマンドで`-flto=thin /O2 /Ob2 /MT /Z7`、`NDEBUG`、PGO指定なしを確認。静的CRT・compiler-rt builtins、`/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO`。元のmanifestを`mt.exe`で再埋め込みし、抽出内容を照合。
- Aは前回の独立保存済みexe/PDBをSHA-256照合して再利用。Bは対象commitの製品ソースから同じレシピでビルドし、全781テスト・10,751,667 assertions成功。計測フックはコピーTUのみ。`StepImpl`・`RetiredPlan`・`HyperpathLookupCandidates`・`PlanPrecomputed`を対応PDBで事前確認。
- 同じkernel CPU専用File profile、1ms、1,024×1,024KiB。pilot成功後に採取。全8本でevents/buffers欠落0、両マーカーあり、kernel区間監査で競合ビルド・テスト・ゲームなし。
- WPA製品関数の未解決0。圧縮CSVと生CSVのハッシュ、exclusive時間・サンプル保存則、支払い整合性を確認。InvalidEvent警告はすべて採取マーカー外。
- canonicalは64bit digestを含めPython整数で24項目を比較。通常indexの性能計測であり、validateの成功判定は含まない。
- 生ETL8本・終了セーブ・Bのexe/PDBを作業ツリー外へ独立コピー。Aの既存独立バックアップも照合した。固定入力は既存ZIPを再利用。[保管manifest](measurements/passenger-wpr-abba-6c11c67f-2026-10-19/recording-backup.json)、[入力ZIP](measurements/passenger-wpr-abba-6c11c67f-2026-10-19/input-backup.json)。

[比較JSON](measurements/passenger-wpr-abba-6c11c67f-2026-10-19/comparison.json)、[指定関数の各本CSV](measurements/passenger-wpr-abba-6c11c67f-2026-10-19/focus-runs.csv)、[全関数のA/B平均差CSV](measurements/passenger-wpr-abba-6c11c67f-2026-10-19/function-before-after.csv)、[canonical](measurements/passenger-wpr-abba-6c11c67f-2026-10-19/canonical-comparison.json)、[採用検証](measurements/passenger-wpr-abba-6c11c67f-2026-10-19/validation.json)。

各ラウンドの`legacy-functions.csv`／`hyperpath-functions.csv`にinclusive/exclusive、各モード配下にgzipの生WPA CSVを保存した。checksは当日のパス・版を含む再現資料であり、その場所から実行する汎用スクリプトではない。
