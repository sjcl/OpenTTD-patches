# 0b8a21c0: 編成内Hyperpath判定再利用の実マップ計測

同じ固定済み `gc3833d807.sav` で、`0b8a21c0f3dc9a24ca0d9d3581c19fb9c207e684` と `97cdc0ad4c9493daea0db6079e90a03924d96b56` を各Legacy・Hyperpath、2,048 tickずつ記録した（2026-10-19 JST）。
ETWの欠落対策で採取プロファイルを変更したため、**97cdc0adも同じ新しいプロファイルで追加測定し、同条件同士を主比較にした**。各組合せ1回、採用WPRは計4本。製品ソースへの追加変更なし。

**validateは不合格: memoのviolations=13,655（validated=13,769,660）。0ではなく、この実マップで再利用の正しさは確認できなかった。**
終了canonicalの一致は、このmemo検証の不一致を打ち消すものではない。

**新版両モード・追加基準両モードとも、終了canonical 24項目が以前の97cdc0adの同じモードと完全一致した。**

64 bit digestを含めPython整数で比較し、浮動小数点には変換していない。

時間はCPU Usage (Sampled)のうち、メインスレッドで `StateGameLoop` を含むスタックのinclusive CPU ms/tick。スタック内の同じ関数は1回だけ数え、親子inclusiveは加算しない。

| モード / 関数 | 同条件97cdc0ad | 0b8a21c0 生値 | 生の変化 | 0b8a21c0 補正後 | 補正後の変化 | samples 旧→新 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| hyperpath / `MayUseHyperpathPolicy` | 0.693739 | 0.638975 | -7.89% | 0.582173 | -16.08% | 1,421 → 1,309 |
| hyperpath / `VisitBoardingCandidates<CargoLoad…>` | 1.987850 | 1.853993 | -6.73% | 1.689183 | -15.02% | 4,071 → 3,798 |
| hyperpath / `StepHyperpath` | 0.722366 | 0.455521 | -36.94% | 0.415027 | -42.55% | 1,478 → 932 |
| hyperpath / StepHyperpath配下のvector再確保 | 0.255056 | 0.020999 | -91.77% | 0.019133 | -92.50% | 522 → 43 |
| hyperpath / 再確保の最大成分 | 0.223239 | 0.007823 | -96.50% | 0.007128 | -96.81% | 457 → 16 |
| legacy / `MayUseHyperpathPolicy` | 1.530621 | 1.068823 | -30.17% | 0.993007 | -35.12% | 3,136 → 2,189 |
| legacy / `VisitBoardingCandidates<CargoLoad…>` | 2.873577 | 2.367355 | -17.62% | 2.199430 | -23.46% | 5,887 → 4,848 |

loading walkはCargoLoad用VisitBoardingCandidates本体だけで、内部lambdaを足していない。完全なシンボル名はfocus.json。
vector再確保はStepHyperpathの子に `std::vector` の `realloc` を含むフレームがあるスタックを1回だけ数えた。内訳はstep-vector-reallocation.json。サンプル0は実行時間0の証明ではない。

約0.22 ms/tickだった最大成分は、PDB上で `std::vector<LinkGraph::EdgeRoute>::_Emplace_reallocate` と表示される成分。上表の「最大成分」は同じシンボルを新旧で照合した値で、再確保合計の内数。

過去の通常CPUプロファイルによる97cdc0adの参考値（今回の主比較の基準値とは採取条件が異なる）:

| Hyperpath関数 | 前回97cdc0ad | 今回同条件97cdc0ad | 今回0b8a21c0 |
| --- | ---: | ---: | ---: |
| MayUseHyperpathPolicy | 0.680144 | 0.693739 | 0.638975 |
| loading walk | 1.940269 | 1.987850 | 1.853993 |
| StepHyperpath | 0.721010 | 0.722366 | 0.455521 |
| StepHyperpath配下vector再確保 | 0.239463 | 0.255056 | 0.020999 |

補正は `utils/passenger_wpr_control.py` の既定分類。untouched時間の新/旧比を機械速度変動と仮定し、新版時間を割った参考値で、CPU周波数の直接測定ではない。測定ばらつきを含む。

| モード | 補正係数 新/旧 | tick実時間 ms/tick 同条件97→0b | sampled CPU合計差 生値 | 補正後 |
| --- | ---: | ---: | ---: | ---: |
| legacy | 1.076349538 | 28.733400 → 32.218429 | +1.187396 | -0.691370 |
| hyperpath | 1.097567902 | 31.852548 → 34.665158 | +1.708230 | -0.797430 |

実マップvalidate:

```text
Passenger hyperpath memo: hits=0; validated=13769660; violations=13655
```

再計算照合 13,769,660件、違反 13,655件。validate終了canonicalも以前の97cdc0ad Hyperpathと24項目完全一致。
このviolationsカウンターには、保存済みmemo値と再計算値の不一致、およびuniqueとしたグループの重複検出の2か所が加算する。今回の集計値だけではどちらが原因かを分離できない（cargopacket.cppのhyperpath_memo_violations加算箇所）。
同じ入力から別の2,048 tickとして `--hyperpath-filter-mode validate` で実行した。WPRなしで、validateの時間を性能比較に使っていない。

条件と検証:

- 同じセーブ・設定・245アセットをSHA-256で確認。StateTicks 3,197,276,448 → 3,197,278,496、実tickサンプル各2,048。
- 新旧とも `--resume --precomputed --warm-precomputed --wait-background-ready`、Hyperpath索引ON、メインスレッドaffinity mask=1。旧版Legacy→旧版Hyperpath→新版Legacy→新版Hyperpathの順。
- 新版はロード手順変更に合わせ、停止中に `--wait-saved-replay` で復元完了を待ってから設定・warm-upを行った。全4本でwarm-up成功・背景処理完了・準備前後のtick不変を確認。追加の定常化・cold・全再構築なし。
- 新旧ともMSVC 19.44 x64 Release、`/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert ON、LTO OFF、静的CRT。旧版は以前保存したexe/PDBのSHA-256と一致を確認して使用。
- 新版は全777 nativeテスト・10,745,559 assertionsが通った。
- 全4本で同一の `cpu-minimal.wprp`。File方式、1ms間隔、system buffersは1,024×1,024KiB。kernelのCpuConfig・Loader・ProcessThread・SampledProfileとSampledProfileスタックだけを採取した。WPAの表示・集計プロファイルも同じ。
- 全4本でlost events / buffersが0、開始・終了マーカーあり。kernelプロセス監査で競合ビルド・テスト・ゲームなし。製品関数の未解決シンボルなし。
- 生CSVのSHA-256、スタックとexclusiveの時間・サンプル数の保存則、支払い整合性を検証。WPA警告は測定マーカー外であることを確認した。

記録方法を変更した理由:

通常CPUプロファイルでは、復元未完了の試行、Legacyの開始マーカー欠落、File方式のイベント欠落が生じた。system/eventバッファ拡大とCSwitch等の除外でも欠落が残ったため、ユーザーイベントコレクターを含めて除いたkernel CPU専用プロファイルへ変更した。
短い試験で欠落0と両マーカーを確認した後、実マップを新旧同一条件で取り直した。欠落ありの記録を時間比較には使っていない。[MicrosoftのETWセッション説明](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/sessions)と[Xperfの欠落時の注意](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/xperf/processing)も参照。
不採用分はメタデータ・ログをchecks配下に残し、大きな生ファイル約29.6GBを削除した。failed-raw-cleanup.jsonに対象・容量を記録した。
計測コピーのwarm-up挿入位置は、追加された準備コマンドのタイマーと区別するため限定した。挿入処理は以前と同じで、製品ソースは変更していない（prepare-compat.json）。

JP+ Multiple UnitsのNewGRF警告が出るセーブをそのまま使用。警告を抑制する変更はしていない。各実行のstderrと、測定フックによるerror pause解除の記録を保存している。
採用した4本のETL・新旧exe/PDBは作業ツリー外にも保存し、recording-backup.jsonに場所・SHA-256を記録した。固定入力は既存の同一ZIPを参照し、重複ZIPは作っていない。

[関数別差分CSV](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/function-before-after.csv)、[注目関数](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/focus.json)、[canonical照合](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/canonical-comparison.json)、[validate](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/validate-result.json)、[全検証](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/validation.json)。

補正の再計算:

```powershell
python -B utils/passenger_wpr_control.py docs/measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/control-97cdc0ad docs/measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19
```
