# 6c11c67f / 55f1cfe0 Clang ThinLTO ABBA・validate計測

A=`6c11c67fd08115ce370fbfb182cdc952f37d75fd`、B=`55f1cfe00796868f1f38215ccc083894b764ee6e`。2026-10-20 JST、同じ凍結済み`gc3833d807.sav`からLegacy／Hyperpathを各A→B→B→A、計8本、各2,048 tick採取した。
**終了canonical 24項目は8本すべて6c11c67fの同じモードと完全一致。先行したB Hyperpath validateも完全一致し、memo・lookupのviolations=0、lookup_duplicates=0だった。**
0d99a67fで観測したcanonical不一致は、この修正版・入力・区間では再発しなかった。決定性や同期の全条件をこの1マップだけで証明するものではない。

両版ともClangCL/LLD 23.1.3、ThinLTO、assert OFF、PGO OFF、`/O2 /Ob2 /MT /Z7`、compiler-rt builtins、同じmanifestをmt.exeで再埋め込みした。
時間は主にメインスレッドの`StateGameLoop`を含むSampledProfileのCPU ms/tick。各版2本の平均・実測最小～最大と合計samplesを示す。tick実時間だけはrunnerのwall timer。

## 指定箇所と全体

- Hyperpath `MayUseHyperpathPolicy` inclusive: 0.553363 → 0.258497 ms/tick（-53.29%）。
- Hyperpath `ResolveHyperpath / Board union` inclusive: 0.230685 → 0.220423 ms/tick（-4.45%）。
- Hyperpath `SharedHyperpathPlan` self: 0.051745 → 0.002442 ms/tick（-95.28%）。
- simulation CPU全体: Hyperpath 19.221183 → 17.405307 ms/tick（-9.45%）、Legacy 20.434627 → 18.349753 ms/tick（-10.20%）。

`MayUseHyperpathPolicy`はA 0.493533～0.613192、B 0.244097～0.272897 ms/tick、`SharedHyperpathPlan` selfはA 0.044921～0.058569、B 0.001954～0.002931で、今回の2本ずつではどちらも範囲が重ならず減少した。
`Resolve / Board union`はA 0.204558～0.256813、B 0.204067～0.236779と重なる。`Board`単体は0.191381→0.156959（−17.99%）だが、union全体の約4.45%を明確な短縮とは断定しない。
Aでは`BoardHyperpath`の独立したフレームが観測されず、その行の0は新規コストとの差を意味しない。親を含むunionを比較する。

**全体CPUはHyperpath −9.45%、Legacy −10.20%という生値だが、最適化単独の効果とは断定できない。**
同じA Hyperpathの実測が15.881876→22.560490 ms/tickと大きく変動し、A/Bの範囲も重なった。ABBAでも今回の変動を十分に除けたとは言えず、局所的な削減とtick全体の改善を区別する。

`Resolve / Board union`は`ResolveHyperpath`・`PassengerServiceDatabase::BoardHyperpath`・`PassengerPrecomputedTable::Board`のいずれかを含むスタックの和集合。親子のinclusiveを重複加算していない。
selfはスタック末端だけ。LTOやICFで境界が変わるため、0サンプルは処理時間0の証明ではない。版ごとの生値・範囲とサンプル数を優先し、各2本だけで小さな差の再現性を断定しない。untouched補正は行っていない。

## Hyperpath

| 対象 | A 平均 [範囲] | B 平均 [範囲] | B−A | 変化 | samples A/B |
| --- | ---: | ---: | ---: | ---: | ---: |
| `StateGameLoop` | 19.221183 [15.881876, 22.560490] | 17.405307 [15.537370, 19.273243] | -1.815876 | -9.45% | 78,712 / 71,276 |
| `simulation_wall_ms_per_tick` | 19.394757 [16.008810, 22.780705] | 17.600488 [15.676856, 19.524120] | -1.794269 | -9.25% | — |
| `MayUseHyperpathPolicy` | 0.553363 [0.493533, 0.613192] | 0.258497 [0.244097, 0.272897] | -0.294865 | -53.29% | 2,267 / 1,059 |
| `MayUseHyperpathPolicy self` | 0.145716 [0.141070, 0.150362] | 0.064933 [0.058098, 0.071769] | -0.080783 | -55.44% | 597 / 266 |
| `ResolveHyperpath` | 0.230685 [0.204558, 0.256813] | 0.220423 [0.204067, 0.236779] | -0.010262 | -4.45% | 945 / 903 |
| `BoardHyperpath` | 0.000000 [0.000000, 0.000000] | 0.171607 [0.164032, 0.179182] | +0.171607 | — | 0 / 703 |
| `Board` | 0.191381 [0.167942, 0.214821] | 0.156959 [0.149388, 0.164531] | -0.034422 | -17.99% | 784 / 643 |
| `ResolveHyperpath / Board union` | 0.230685 [0.204558, 0.256813] | 0.220423 [0.204067, 0.236779] | -0.010262 | -4.45% | 945 / 903 |
| `SharedHyperpathPlan self` | 0.051745 [0.044921, 0.058569] | 0.002442 [0.001954, 0.002931] | -0.049303 | -95.28% | 212 / 10 |
| `SharedHyperpathPlan` | 0.109116 [0.090816, 0.127415] | 0.079349 [0.074729, 0.083970] | -0.029766 | -27.28% | 447 / 325 |
| `RetiredPlan` | 0.072988 [0.058590, 0.087386] | 0.053461 [0.047847, 0.059075] | -0.019527 | -26.75% | 299 / 219 |
| `PlanPrecomputed` | 0.191434 [0.154478, 0.228390] | 0.190440 [0.177886, 0.202995] | -0.000994 | -0.52% | 783 / 779 |
| `StepImpl self` | 0.011261 [0.008800, 0.013722] | 0.008553 [0.008307, 0.008798] | -0.002708 | -24.05% | 46 / 35 |
| `TickPrecomputed` | 0.109445 [0.090532, 0.128358] | 0.090031 [0.085660, 0.094403] | -0.019414 | -17.74% | 447 / 368 |

## Legacy

| 対象 | A 平均 [範囲] | B 平均 [範囲] | B−A | 変化 | samples A/B |
| --- | ---: | ---: | ---: | ---: | ---: |
| `StateGameLoop` | 20.434627 [19.216500, 21.652754] | 18.349753 [16.667455, 20.032050] | -2.084875 | -10.20% | 83,692 / 75,155 |
| `simulation_wall_ms_per_tick` | 20.553377 [19.307827, 21.798928] | 18.811151 [17.477460, 20.144842] | -1.742226 | -8.48% | — |
| `MayUseHyperpathPolicy` | 0.995003 [0.939325, 1.050682] | 0.718461 [0.691379, 0.745544] | -0.276542 | -27.79% | 4,076 / 2,943 |
| `MayUseHyperpathPolicy self` | 0.237743 [0.223112, 0.252374] | 0.124509 [0.120615, 0.128403] | -0.113234 | -47.63% | 974 / 510 |
| `ResolveHyperpath` | 0.217030 [0.198235, 0.235826] | 0.202853 [0.179682, 0.226023] | -0.014178 | -6.53% | 889 / 831 |
| `BoardHyperpath` | 0.000000 [0.000000, 0.000000] | 0.167216 [0.147470, 0.186961] | +0.167216 | — | 0 / 685 |
| `Board` | 0.174064 [0.162597, 0.185532] | 0.165751 [0.146983, 0.184520] | -0.008313 | -4.78% | 713 / 679 |
| `ResolveHyperpath / Board union` | 0.217030 [0.198235, 0.235826] | 0.202853 [0.179682, 0.226023] | -0.014178 | -6.53% | 889 / 831 |
| `SharedHyperpathPlan self` | 0.002686 [0.001465, 0.003907] | 0.006102 [0.004393, 0.007811] | +0.003416 | +127.18% | 11 / 25 |
| `SharedHyperpathPlan` | 0.002686 [0.001465, 0.003907] | 0.006102 [0.004393, 0.007811] | +0.003416 | +127.18% | 11 / 25 |
| `RetiredPlan` | 0.390861 [0.364716, 0.417005] | 0.460900 [0.436976, 0.484823] | +0.070039 | +17.92% | 1,601 / 1,888 |
| `PlanPrecomputed` | 0.441148 [0.413770, 0.468526] | 0.418047 [0.400903, 0.435191] | -0.023101 | -5.24% | 1,806 / 1,712 |
| `StepImpl self` | 0.096354 [0.092421, 0.100287] | 0.087046 [0.081152, 0.092939] | -0.009308 | -9.66% | 394 / 356 |
| `TickPrecomputed` | 0.167741 [0.159383, 0.176099] | 0.155997 [0.145171, 0.166822] | -0.011744 | -7.00% | 686 / 638 |

LegacyにはHyperpath専用のtick間共有が適用されないため、同じ短縮を期待する対照として扱っていない。

## 各本の生値

| 順序 | 版 | モード | simulation CPU | tick実時間 | MayUse inclusive | Resolve/Board union | Shared self |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 01-A1/legacy | A | legacy | 19.216500 | 19.307827 | 0.939325 | 0.198235 | 0.001465 |
| 01-A1/hyperpath | A | hyperpath | 15.881876 | 16.008810 | 0.493533 | 0.204558 | 0.044921 |
| 02-B1/legacy | B | legacy | 16.667455 | 17.477460 | 0.691379 | 0.179682 | 0.004393 |
| 02-B1/hyperpath | B | hyperpath | 15.537370 | 15.676856 | 0.244097 | 0.204067 | 0.001954 |
| 03-B2/legacy | B | legacy | 20.032050 | 20.144842 | 0.745544 | 0.226023 | 0.007811 |
| 03-B2/hyperpath | B | hyperpath | 19.273243 | 19.524120 | 0.272897 | 0.236779 | 0.002931 |
| 04-A2/legacy | A | legacy | 21.652754 | 21.798928 | 1.050682 | 0.235826 | 0.003907 |
| 04-A2/hyperpath | A | hyperpath | 22.560490 | 22.780705 | 0.613192 | 0.256813 | 0.058569 |

各ラウンド内はLegacy→Hyperpath、ラウンドはA1→B1→B2→A2。全て同じ入力から新規プロセスで開始した。ABBAは時間ドリフトの偏りを抑える順序であり、上の範囲は信頼区間ではない。

## validate（B Hyperpath、1本）

WPR前に同じ固定入力から`--hyperpath-filter-mode validate --validation-only`を1回実行した。性能の平均には含めていない。2,048 tickと準備完了・準備中tick不変を確認。

| 診断 | validated | conservative | violations | lookup_duplicates |
| --- | ---: | ---: | ---: | ---: |
| memo | 13,783,197 | 9,088 | 0 | 0 |
| lookup | 1,038,407 | — | 0 | — |

canonical 24項目は6c11c67f Hyperpathと完全一致。違反カウンター条件とcanonical条件の両方が合格した。`simulation_timing_valid=false`はvalidation-onlyの意図した値。

## 条件・採用検査・保管

- 固定入力46,620,248 bytes、SHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットも旧manifestに照合。StateTicks **3,197,276,448 → 3,197,278,496**。
- `--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。通常の準備だけで、追加warm-up tick・cold・再構築指定なし。validateだけ上記の別モード。
- Aは独立保存済み6c11c67f exe/PDBをハッシュ照合して再利用。Bは正確な55f1cfe0 checkoutから同じレシピでビルドした後、元の作業ブランチへ復帰。計測用フックはコピーTUだけに挿入した。
- Bの全782 nativeテスト・11,281,988 assertions成功。実clang-clコマンド、bitcode、compiler-rt builtins、抽出manifestと対応PDBを採取前に確認。
- 同じkernel CPU専用File profile、1ms、1,024×1,024KiB。pilot合格後に直列採取。採用8本すべてevents/buffers欠落0・両マーカーあり・kernel区間監査で競合ビルド／テスト／ゲームなし。関連チャットへ採取枠を事前調整した。
- WPA製品関数の未解決0。CSVのgzip展開ハッシュ、exclusive時間・サンプル保存則、支払い整合性を確認。InvalidEvent警告は測定マーカー外。
- ETL8本・終了セーブ9本・B exe/PDBを作業ツリー外へ独立コピー。Aの既存独立バックアップも再照合。固定入力ZIPは再利用。[保管manifest](measurements/passenger-wpr-abba-55f1cfe0-2026-10-20/recording-backup.json)、[入力ZIP](measurements/passenger-wpr-abba-55f1cfe0-2026-10-20/input-backup.json)。

[指定関数CSV](measurements/passenger-wpr-abba-55f1cfe0-2026-10-20/focus-runs.csv)、[全関数A/B差CSV](measurements/passenger-wpr-abba-55f1cfe0-2026-10-20/function-before-after.csv)、[canonical](measurements/passenger-wpr-abba-55f1cfe0-2026-10-20/canonical-comparison.json)、[validate](measurements/passenger-wpr-abba-55f1cfe0-2026-10-20/validate-result.json)、[採用検査](measurements/passenger-wpr-abba-55f1cfe0-2026-10-20/validation.json)、[比較JSON](measurements/passenger-wpr-abba-55f1cfe0-2026-10-20/comparison.json)。
各ラウンドの関数CSVにinclusive/exclusive、各モード配下のgzipに生WPA CSVを保存。checksは当日のパスを含む再現資料で、その場所から実行する汎用スクリプトではない。
