# 9ab0228d: LocalPolicy共有キャッシュの実マップWPR

`9ab0228db17178ef931de961305ebec544162f6a` で、前回と同じ固定済み `gc3833d807.sav` からLegacyとHyperpathを各2,048 tick、有効な記録を1本ずつ取得した。
計測は2026-10-18に開始し、10-19に終了した（JST）。比較対象は前回のf9c4a85b。

**両モードとも、終了canonical 24項目すべてが前回の同じモードと完全一致した。**
64 bit digestもPythonの整数として比較し、浮動小数点へ変換していない。

| モード | 前回とのcanonical一致 | 項目数 |
| --- | --- | ---: |
| legacy | 完全一致 | 24 |
| hyperpath | 完全一致 | 24 |

CPU Usage (Sampled)の `StateGameLoop` を含むスタックが対象。各スタック内で同じ関数を1回だけ数えたinclusive。
表の親子関数のinclusiveは加算しない。単位はCPU ms/tick。

| モード / 関数 | f9c4a85b | 9ab0228d 生値 | 生の変化 | 9ab0228d 補正後 | 補正後の変化 |
| --- | ---: | ---: | ---: | ---: | ---: |
| hyperpath / `LocalPolicy` | 0.923347 | 0.146495 | -84.13% | 0.140399 | -84.79% |
| hyperpath / `CanUseHyperpathPolicy` | 0.988749 | 0.305139 | -69.14% | 0.292442 | -70.42% |
| legacy / `LocalPolicy` | 0.561526 | 0.105459 | -81.22% | 0.111547 | -80.13% |
| legacy / `CanUseHyperpathPolicy` | 0.676257 | 0.269021 | -60.22% | 0.284551 | -57.92% |

`CanUseHyperpathPolicy` は `PassengerJourneyLoadingContext` のメソッド。
新しい `StationChoices` は `LocalPolicy` から呼ばれるため、そのコストも上のLocalPolicy inclusiveに含まれる。
StationChoicesへ移した処理を計測対象から外していない。

| モード | LocalPolicy前回 → 今回 samples | CanUseHyperpathPolicy前回 → 今回 samples | 新StationChoices inclusive CPU ms/tick / samples |
| --- | ---: | ---: | ---: |
| hyperpath | 1,891 → 300 | 2,025 → 625 | 0.064443 / 132 |
| legacy | 1,150 → 216 | 1,385 → 551 | 0.050295 / 103 |

補正には `utils/passenger_wpr_control.py` の既定分類を用いた。
untouched時間の比（新/旧）を共通の機械速度変動と仮定し、新版時間をその比で割った。CPU周波数の直接測定ではない。
各モード1回のサンプリング測定であり、小さい差には測定ばらつきが含まれる。

| モード | 補正係数 新/旧 | tick全体 実時間 ms/tick 前回 → 今回 |
| --- | ---: | ---: |
| legacy | 0.945423124 | 27.685906 → 27.163088 |
| hyperpath | 1.043418811 | 28.546285 → 30.181674 |

計測条件・検証:

- 同じセーブ・設定・245個のアセットをSHA-256で確認。StateTicks 3,197,276,448 → 3,197,278,496。各2,048個の実tickサンプル。
- 前回と同じ順序（Legacy、Hyperpath）、`--resume --precomputed --warm-precomputed --wait-background-ready`、Hyperpath索引ON、メインスレッドaffinity mask=1。
- 停止中の準備でtickが進んでいないことを確認。再構築・cold条件・追加トレース・新たな定常化なし。
- MSVC 19.44 x64 Release、`/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert ON、LTO OFF。CPU採取項目・スタック設定とWPAツールは前回と同じ。Hyperpathの記録方式は下記のとおり変更した。
- 全763 nativeテスト・10,586,649 assertionsが通った。製品ソースへの追加変更なし。
- 両区間のkernel監査で競合ビルド・テスト・ゲームなし。lost events / buffersは0、製品関数の未解決シンボルなし。
- 生CSVのSHA-256、サンプル数・exclusive時間の保存則、支払い整合性、WPA警告が計測マーカー外にあることを検証。

Hyperpathはメモリ方式の3回で開始マーカーがなく、性能集計から除外した。1秒の準備待ちとイベントバッファ20→256への拡大も試したが、改善しなかった。canonical 24項目は3回とも一致していた。
採用するHyperpath記録は同じ入力・バイナリで `wpr -start CPU -filemode` を使った。開始前の計測区間外に1秒の待ちと準備マーカーを入れた。
**Hyperpathの時間比較は、前回のMemoryと今回のFileという保存方式・バッファ構成の差を含む。** Legacyは前回と同じMemory方式。
CPUプロファイルをエクスポートし、SystemProviderのkeywords・stacks、EventProvider群、各collectorのprovider参照が一致することをXMLで検証した。system buffer設定はMemory 4,781→File 20、event buffer設定は20→20。
ファイル方式とプロファイルのエクスポートは[MicrosoftのWPR CLI仕様](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options)に従った。
設定と一致確認は `recorder-settings.json`、`checks/cpu-memory.wprp` と `checks/cpu-file.wprp`。開始・終了マーカー両方の監査に通った記録だけを採用した。
不採用3回のマーカー表・ログ・プロセスメタデータは `checks/rejected-missing-begin-marker*/` に残し、大きな不採用ETLと重複入力は削除した。

前回同様、JP+ Multiple Unitsの車両容量変更に関するNewGRF警告が出るセーブで計測した。警告を抑制する変更はしていない。

- legacy: NewGRF警告 前回 11,200件 → 今回 11,200件、今回の計測用フックによるerror pause解除 1回。
- hyperpath: NewGRF警告 前回 11,200件 → 今回 11,200件、今回の計測用フックによるerror pause解除 1回。

過去の未追跡データの削除:

ユーザーの追加依頼に従い、過去5つの未追跡作業フォルダーと作業ツリー外の旧記録・入力バックアップ4件を削除した。
Gitに保存された報告書・CSV・canonical・ソース、指定の元セーブ、今回の9ab0228d計測は残した。
必要なWPAツールは今回の作業フォルダーへ移してから削除した。削除処理は2本のWPR記録・保存終了後に実行した。
削除前後のCドライブ空き容量は 9.67 GB → 124.83 GB。
対象パス・件数・検証結果は `previous-results-cleanup.json`。別途、今回の準備中に重複シンボルキャッシュ約16.6 GBも整理した。
**過去の報告書に記載された作業ツリー外の旧ETL・旧入力バックアップは、今回の依頼で削除済み。** Git内のCSVとcanonicalは引き続き参照できる。
今回分だけはETL・対応exe/PDB・固定入力のZIPを作業ツリー外にも保持し、`recording-backup.json` と `input-backup.json` に場所とSHA-256を記録した。

[関数別inclusive/exclusive差分CSV](measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18/function-before-after.csv)、
[注目関数の全数値](measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18/focus.json)、
[canonical照合](measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18/canonical-comparison.json)、
[検証結果](measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18/validation.json)、
[削除結果](measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18/previous-results-cleanup.json)。

補正の再計算:

```powershell
python -B utils/passenger_wpr_control.py docs/measurements/passenger-wpr-generation-f9c4a85b-2026-10-18 docs/measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18
```
