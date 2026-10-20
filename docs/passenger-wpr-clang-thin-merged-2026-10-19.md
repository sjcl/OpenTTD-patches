# jgrpp-ya merge後のLLVM 23 ThinLTO実マップ計測

`sjcl/jgrpp-ya`の`82a587e2c6`を取り込み、merge commit `f97bac717c840852d8240f0eac51a92bad0abb9b`で、同じ固定済み`gc3833d807.sav`からLegacy・Hyperpathを各2,048 tick、WPRを各1本採取した（2026-10-19 JST）。
**終了canonical 24項目は両モードとも97cdc0ad・0b8a21c0・8f829b8cの同じモードと完全一致し、Hyperpathは6aeaff68のvalidateとも完全一致した。**

主比較は前回8f829b8cのMSVC 19.44 / assert ON / LTO OFFと、今回のmerge後LLVM 23.1.3 / assert OFF / ThinLTO。PGOは今回明示的にOFF。
**merge・コンパイラ・assert・LTO設定を合わせた差であり、ThinLTOやコンパイラ単独の効果を分離した比較ではない。**
コンパイラ変更は全コードへ効くため、`passenger_wpr_control.py`の「未変更部分」を機械変動の対照として使わず、生値を報告した。

## tick全体

時間はメインスレッドで`StateGameLoop`を含むWPR SampledProfileのCPU ms/tick。ロード・停止中の準備・保存は除外した。

| モード | sampled CPU 前回MSVC | 今回ThinLTO | 生の変化 | tick実時間 前回→今回 ms/tick |
| --- | ---: | ---: | ---: | ---: |
| legacy | 26.545825 | 24.070563 | -9.32% | 27.336955 → 28.085980 |
| hyperpath | 21.659964 | 24.082862 | +11.19% | 22.350623 → 27.517461 |

各組合せ1回であり、実行間の機械状態・測定ばらつきも含む。最適化・インライン化・ICFで関数境界が変わるため、全体の判断は上表を優先する。
以下の内訳はPDB上で解決された関数・呼出し経路のinclusive CPU ms/tick。0サンプルは処理時間0の証明ではなく、完全なインライン化等で呼出し境界が残らない場合もある。

## Hyperpathの内訳

| 関数 / 経路 | 前回MSVC | 今回Clang ThinLTO | 生の変化 | samples 旧→新 |
| --- | ---: | ---: | ---: | ---: |
| `StationCargoList::Load` | 1.461909 | 1.627325 | +11.32% | 2,994 → 3,332 |
| `Load -> _malloc_base` | 0.105493 | 0.143879 | +36.39% | 216 → 295 |
| `Load -> RtlFreeHeap` | 0.086878 | 0.100637 | +15.84% | 178 → 206 |
| `LoadingContext constructor` | 0.024930 | 0.024899 | -0.12% | 51 → 51 |
| `LoadingContext destructor` | 0.030759 | 0.044903 | +45.98% | 63 → 92 |
| `Load -> LoadingContext constructor` | 0.000000 | 0.000000 | — | 0 → 0 |
| `Load -> LoadingContext destructor` | 0.000000 | 0.000000 | — | 0 → 0 |
| `MayUseHyperpathPolicy` | 0.587370 | 0.649094 | +10.51% | 1,203 → 1,329 |
| `MayUseHyperpathPolicy -> PlanPrecomputed` | 0.087898 | 0.102100 | +16.16% | 180 → 209 |
| `CanUseHyperpathPolicy -> PlanPrecomputed` | 0.007325 | 0.008298 | +13.28% | 15 → 17 |
| `PlanPrecomputed` | 0.267788 | 0.306680 | +14.52% | 548 → 628 |
| `loading walk` | 1.460446 | 比較不可 | — | 2,991 → — |

## Legacyの内訳

| 関数 / 経路 | 前回MSVC | 今回Clang ThinLTO | 生の変化 | samples 旧→新 |
| --- | ---: | ---: | ---: | ---: |
| `StationCargoList::Load` | 2.242646 | 2.196853 | -2.04% | 4,593 → 4,499 |
| `Load -> _malloc_base` | 0.142092 | 0.141532 | -0.39% | 291 → 290 |
| `Load -> RtlFreeHeap` | 0.100084 | 0.096663 | -3.42% | 205 → 198 |
| `LoadingContext constructor` | 0.026875 | 0.024961 | -7.12% | 55 → 51 |
| `LoadingContext destructor` | 0.073284 | 0.066454 | -9.32% | 150 → 136 |
| `Load -> LoadingContext constructor` | 0.000000 | 0.000000 | — | 0 → 0 |
| `Load -> LoadingContext destructor` | 0.000000 | 0.000000 | — | 0 → 0 |
| `MayUseHyperpathPolicy` | 1.101047 | 1.137147 | +3.28% | 2,255 → 2,329 |
| `MayUseHyperpathPolicy -> PlanPrecomputed` | 0.345698 | 0.336987 | -2.52% | 708 → 690 |
| `CanUseHyperpathPolicy -> PlanPrecomputed` | 0.013191 | 0.011196 | -15.12% | 27 → 23 |
| `PlanPrecomputed` | 0.505001 | 0.486318 | -3.70% | 1,034 → 996 |
| `loading walk` | 2.239236 | 比較不可 | — | 4,586 → — |

`A -> B`は同じスタックでAの子孫にBがある経路だけを数えた。1スタック内の重複は1回だけ数え、親子inclusiveを加算していない。
ctor/dtorは全体とLoad配下を分けた。loading walkはCargoLoad用VisitBoardingCandidates本体の境界を用いる。
今回のVisitBoardingCandidates本体はインライン化され、ソース位置付きの内部lambdaが残っている。これらの関数名は解決できているが、旧CargoLoad本体と同じ境界を切り出せないため、loading walkは比較不可とした。

## 指定ビルド条件の検証

- ClangCL / LLD **23.1.3**、Visual Studio 2022 BuildToolsのMSVC ABI・STL・静的CRT、x64 Release。
- `openttd_lib`と`openttd`の実際のclang-clコマンドを検査し、両方で`-flto=thin /O2 /Ob2 /MT /Z7`、`NDEBUG`、PGO指定なしを確認した。CMakeの要求値だけで判断していない。
- `OPTION_USE_ASSERTS=OFF`、`OPTION_PGO=OFF`、`OPTION_NO_LTO=OFF`。自動IPOとの混在なし。専用ThinLTO backend cacheを使用。
- `openttd_lib`の608オブジェクトすべてがLLVM bitcode。exeおよびtestターゲットのbitcodeも確認した。詳細はlto-objects.json。
- compiler-rt builtinsの実リンク指定と実ファイルのSHA-256をactual-compile-flags.jsonに保存。
- `/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO`。SDKの`mt.exe`で元の`os/windows/openttd.manifest`をexe/testへ再埋め込みし、取り出した内容が元と等しいことを確認した。
- `/Z7`付きPDBのmodule symbols / inlinee IDsから`StateGameLoop`、`CanBoard`、`MayUseHyperpathPolicy`、`PlanPrecomputed`、`VisitBoardingCandidates`を採取前に確認。強制noinline等は追加していない。
- 全779 nativeテスト・10,745,600 assertions成功。

## memo・採取品質・保管

| モード | memo hits 前回→今回 | lookup rebuilds | indexed_groups |
| --- | ---: | ---: | ---: |
| legacy | 4,046,826 → 4,046,826 | 2,709 | 195,099 |
| hyperpath | 598,504 → 598,504 | 2,714 | 203,870 |

- セーブSHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットも前回manifestへ照合。変換・再保存なし。
- StateTicks **3,197,276,448 → 3,197,278,496**。差・実サンプル数とも各2,048。canonicalは64bit digestを含めPython整数で照合した。
- `--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。モードだけ切替。準備成功・準備中tick不変、cold・再構築・追加warm-up tickなし。
- 短いpilotが欠落0・両マーカーで成功した後、Legacy→Hyperpathを直列採取。kernel CPU専用File方式、1ms、1,024×1,024KiB。同じWPR/WPA profileをSHA-256で確認した。
- 両本ともlost events / buffers=0、両マーカーあり、kernel区間監査で競合ビルド・テスト・ゲームなし。WPAで製品関数の未解決なし。CSVハッシュ、時間・サンプル保存則、支払い整合性を確認。InvalidEvent警告は測定マーカー外。
- 通常indexモードの計測であり、追加のvalidateは行っていない。既存NewGRF警告とerror pause解除のログも保持した。
- ETL・exe/PDB・終了セーブは作業ツリー外へ独立コピーし、recording-backup.jsonへ場所・容量・SHA-256を記録。固定入力は既存ZIPを再利用した。

[全体集計](measurements/passenger-wpr-clang-thin-merged-2026-10-19/summary.json)、[関数別差分CSV](measurements/passenger-wpr-clang-thin-merged-2026-10-19/function-before-after.csv)、[注目経路](measurements/passenger-wpr-clang-thin-merged-2026-10-19/focus.json)、[canonical照合](measurements/passenger-wpr-clang-thin-merged-2026-10-19/canonical-comparison.json)、[実コンパイル指定](measurements/passenger-wpr-clang-thin-merged-2026-10-19/actual-compile-flags.json)、[PDB事前検証](measurements/passenger-wpr-clang-thin-merged-2026-10-19/pdb-preflight.json)、[品質検証](measurements/passenger-wpr-clang-thin-merged-2026-10-19/validation.json)。

生WPA CSVは各モード配下のgzip。checksのスクリプトは当日のパス・版を含む再現資料であり、その場所から直接実行する汎用runnerではない。
