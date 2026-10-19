# 8f829b8c: Passenger積み込み時のheap churn削減

同じ固定済み `gc3833d807.sav` を使い、`8f829b8c3d5f35cb420c2a16812716ef24e38939` で通常条件のLegacy・Hyperpathを各2,048 tick、WPRを各1本採取した（2026-10-19 JST）。
比較基準は同じkernel CPU専用プロファイルで採った0b8a21c0の保存済み2本。
**終了canonical 24項目は両モードとも97cdc0ad・0b8a21c0の同じモードと完全一致し、Hyperpathは6aeaff68のvalidateとも完全一致した。**
6aeaff68にはLegacyの記録がなく、時間のプロファイルもない。今回の時間差は6aeaff68と8f829b8cの両変更を含み、それぞれの単独効果へは分離していない。

HyperpathのLoad配下では、malloc/freeは補正後も減った。Load配下のコンストラクタ／デストラクタは今回0サンプルだった。一方、`MayUseHyperpathPolicy -> PlanPrecomputed` は生値では減ったが、補正後は前回を下回らなかった。
Hyperpathの未変更部分の新/旧比は約0.761と大きく変動した。全体のsampled CPU差は生値−6.527 ms/tick、補正後+0.264 ms/tickであり、この1本ずつからtick全体の高速化は確認できない。

時間はメインスレッドで `StateGameLoop` を含むサンプルのinclusive CPU ms/tick。`A -> B` は同じスタックでAの子孫にBがある経路だけを数えた。
1スタック内の重複は1回だけ数え、親子inclusiveを足していない。

## Hyperpath

| 関数 / 呼出し経路 | 0b8a21c0 | 8f829b8c 生値 | 生の変化 | 補正後 | samples 旧→新 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `StationCargoList::Load` | 1.880816 | 1.461909 | -22.27% | 1.920232 | 3,853 → 2,994 |
| `Load -> _malloc_base` | 0.236822 | 0.105493 | -55.45% | 0.138567 | 485 → 216 |
| `Load -> RtlFreeHeap` | 0.136203 | 0.086878 | -36.21% | 0.114115 | 279 → 178 |
| `LoadingContext constructor` | 0.045912 | 0.024930 | -45.70% | 0.032745 | 94 → 51 |
| `LoadingContext destructor` | 0.049371 | 0.030759 | -37.70% | 0.040403 | 101 → 63 |
| `Load -> LoadingContext constructor` | 0.016575 | 0.000000 | -100.00% | 0.000000 | 34 → 0 |
| `Load -> LoadingContext destructor` | 0.007334 | 0.000000 | -100.00% | 0.000000 | 15 → 0 |
| `MayUseHyperpathPolicy` | 0.638975 | 0.587370 | -8.08% | 0.771516 | 1,309 → 1,203 |
| `MayUseHyperpathPolicy -> PlanPrecomputed` | 0.107828 | 0.087898 | -18.48% | 0.115455 | 221 → 180 |
| `CanUseHyperpathPolicy -> PlanPrecomputed` | 0.019535 | 0.007325 | -62.50% | 0.009621 | 40 → 15 |
| `PlanPrecomputed` | 0.332444 | 0.267788 | -19.45% | 0.351742 | 681 → 548 |
| `loading walk` | 1.853993 | 1.460446 | -21.23% | 1.918311 | 3,798 → 2,991 |

## Legacy

| 関数 / 呼出し経路 | 0b8a21c0 | 8f829b8c 生値 | 生の変化 | 補正後 | samples 旧→新 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `StationCargoList::Load` | 2.394660 | 2.242646 | -6.35% | 2.209165 | 4,904 → 4,593 |
| `Load -> _malloc_base` | 0.230985 | 0.142092 | -38.48% | 0.139971 | 473 → 291 |
| `Load -> RtlFreeHeap` | 0.123008 | 0.100084 | -18.64% | 0.098590 | 252 → 205 |
| `LoadingContext constructor` | 0.050837 | 0.026875 | -47.14% | 0.026473 | 104 → 55 |
| `LoadingContext destructor` | 0.057592 | 0.073284 | +27.25% | 0.072189 | 118 → 150 |
| `Load -> LoadingContext constructor` | 0.019045 | 0.000000 | -100.00% | 0.000000 | 39 → 0 |
| `Load -> LoadingContext destructor` | 0.007287 | 0.000000 | -100.00% | 0.000000 | 15 → 0 |
| `MayUseHyperpathPolicy` | 1.068823 | 1.101047 | +3.01% | 1.084609 | 2,189 → 2,255 |
| `MayUseHyperpathPolicy -> PlanPrecomputed` | 0.411060 | 0.345698 | -15.90% | 0.340536 | 842 → 708 |
| `CanUseHyperpathPolicy -> PlanPrecomputed` | 0.035678 | 0.013191 | -63.03% | 0.012994 | 73 → 27 |
| `PlanPrecomputed` | 0.600901 | 0.505001 | -15.96% | 0.497462 | 1,231 → 1,034 |
| `loading walk` | 2.367355 | 2.239236 | -5.41% | 2.205806 | 4,848 → 4,586 |

コンストラクタ／デストラクタの表はシミュレーション全体のinclusive。Load配下だけの値もfocus.jsonに保存した。
loading walkはCargoLoad用VisitBoardingCandidates本体。`_malloc_base` / `RtlFreeHeap` はLoad配下だけであり、プロセス全体のheap合計ではない。
サンプル0は実行時間0の証明ではない。インライン化・ICFによるシンボル帰属差にも留意する。

## memoと全体時間

| モード | memo hits 0b8a21c0 → 8f829b8c | lookup rebuilds | indexed_groups |
| --- | ---: | ---: | ---: |
| legacy | 4,028,203 → 4,046,826 | 2,709 | 195,099 |
| hyperpath | 568,955 → 598,504 | 2,714 | 203,870 |

通常indexモードの記録で、validateは追加実行していない。診断の生値はcapture-results.json / comparison.jsonとコンソール全文に保存した。

| モード | tick実時間 ms/tick 旧→新 | sampled CPU ms/tick 旧→新 | 補正係数 | 補正後CPU差 ms/tick |
| --- | ---: | ---: | ---: | ---: |
| legacy | 32.218429 → 27.336955 | 26.486199 → 26.545825 | 1.015155708 | -0.336688 |
| hyperpath | 34.665158 → 22.350623 | 28.186853 → 21.659964 | 0.761318726 | +0.263732 |

補正は `passenger_wpr_control.py` のuntouched新/旧比を使った参考値で、CPU周波数の直接測定ではない。生値を併記した。各組合せ1回のため測定ばらつきを含む。

## 条件・品質・保管

- 固定セーブSHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`、cfgと245アセットも前回manifestに照合。変換・再保存なし。
- StateTicks 3,197,276,448 → 3,197,278,496、実tick差・サンプル数とも各2,048。全canonicalをPython整数で照合。
- `--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。モードだけ切替。停止中replay・warm・背景準備が成功し、準備前後のtick不変。cold・再構築・追加warm-up tickなし。
- MSVC 19.44 x64 Release、静的CRT、assert ON、LTO OFF、前回と同じフラグとコピーTUの計測フック。製品コードへの追加変更なし。全778 nativeテスト・10,745,598 assertions成功。
- 短いpilotの後、Legacy→Hyperpathを直列採取。kernel CPU専用File方式、1ms、1,024×1,024KiBのsystem buffer。同一WPR/WPA profileをSHA-256で確認。
- 両本のlost events / buffers=0、両マーカーあり、kernel区間監査で競合ビルド・テスト・ゲームなし。製品関数の未解決なし。CSVハッシュと時間・サンプル保存則、支払い整合性を検証。WPAのInvalidEvent警告はマーカー外。
- 初回pilotのrecorder起動失敗は小さいログを保存。一時フォルダー事前作成とWindows PowerShellのstderr取得を修正し、短いpilotを再確認してから本番へ進んだ。本番の2本は各1回。既存のNewGRF警告とerror pause解除のログも保持。
- 採用ETL・exe/PDB・終了セーブは作業ツリー外に独立コピーし、recording-backup.jsonへ場所・容量・SHA-256を記録。固定入力は既存ZIPを再利用した。

[注目経路](measurements/passenger-wpr-loading-scratch-8f829b8c-2026-10-19/focus.json)、[関数別inclusive/exclusive差分CSV](measurements/passenger-wpr-loading-scratch-8f829b8c-2026-10-19/function-before-after.csv)、[canonical照合](measurements/passenger-wpr-loading-scratch-8f829b8c-2026-10-19/canonical-comparison.json)、[補正計算](measurements/passenger-wpr-loading-scratch-8f829b8c-2026-10-19/control-0b8a21c0.json)、[品質検証](measurements/passenger-wpr-loading-scratch-8f829b8c-2026-10-19/validation.json)。
生WPA CSVは各モード配下のgzip。checks内のスクリプトは当日のパス・版を含む再現資料で、その場所から直接実行する汎用ツールではない。

補正の再計算:

```powershell
python -B utils/passenger_wpr_control.py docs/measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19 docs/measurements/passenger-wpr-loading-scratch-8f829b8c-2026-10-19
```
