# 97cdc0ad: 待機パケット索引更新の実マップWPR

`97cdc0ad4c9493daea0db6079e90a03924d96b56` で、前回と同じ固定済み `gc3833d807.sav` からLegacyとHyperpathを各2,048 tick、1本ずつ計測した（2026-10-19 JST）。
比較対象は9ab0228d。製品ソースへの追加変更はしていない。

**両モードとも、終了canonical 24項目すべてが9ab0228dの同じモードと完全一致した。**

- legacy: 24項目、完全一致。
- hyperpath: 24項目、完全一致。

64 bit digestもPython整数として比較し、浮動小数点には変換していない。

時間はCPU Usage (Sampled)のうち、メインスレッドで `StateGameLoop` を含むスタックを対象にしたinclusive CPU ms/tick。
同じスタック内の同じ関数は1回だけ数え、親子関数のinclusiveを加算しない。6関数はいずれも `StationCargoList` のメソッド。

| モード / 関数 | 9ab0228d | 97cdc0ad 生値 | 生の変化 | 補正後 | 補正後の変化 | samples 前回→今回 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| hyperpath / `RoutePassengerJourneys` | 1.335272 | 0.882041 | -33.94% | 0.880403 | -34.07% | 2,730 → 1,803 |
| hyperpath / `RecordCapacityMiss` | 0.467764 | 0.276400 | -40.91% | 0.275886 | -41.02% | 958 → 566 |
| hyperpath / `ForgetWaitingPacket` | 0.883714 | 0.324309 | -63.30% | 0.323707 | -63.37% | 1,808 → 663 |
| hyperpath / `IndexPacket` | 0.704243 | 0.288426 | -59.04% | 0.287890 | -59.12% | 1,441 → 590 |
| hyperpath / `AddBoardingEntry` | 0.337703 | 0.160347 | -52.52% | 0.160050 | -52.61% | 691 → 328 |
| hyperpath / `RemoveBoardingEntry` | 0.374316 | 0.155539 | -58.45% | 0.155251 | -58.52% | 766 → 318 |
| legacy / `RoutePassengerJourneys` | 1.018901 | 0.785461 | -22.91% | 0.735843 | -27.78% | 2,083 → 1,605 |
| legacy / `RecordCapacityMiss` | 0.264661 | 0.219706 | -16.99% | 0.205827 | -22.23% | 542 → 450 |
| legacy / `ForgetWaitingPacket` | 0.674551 | 0.293374 | -56.51% | 0.274841 | -59.26% | 1,380 → 600 |
| legacy / `IndexPacket` | 0.552405 | 0.299588 | -45.77% | 0.280663 | -49.19% | 1,130 → 613 |
| legacy / `AddBoardingEntry` | 0.268410 | 0.170647 | -36.42% | 0.159867 | -40.44% | 549 → 349 |
| legacy / `RemoveBoardingEntry` | 0.262414 | 0.133424 | -49.16% | 0.124996 | -52.37% | 537 → 273 |

サンプル0は実行時間0の証明ではない。インライン展開や短い処理では採取されない場合がある。
補正は `utils/passenger_wpr_control.py` の既定分類による。untouched時間の新/旧比を機械速度変動と仮定し、新版時間をその比で割った参考値で、CPU周波数の直接測定ではない。
各モード1回の測定であり、測定ばらつきが含まれる。

| モード | 補正係数 新/旧 | tick全体 実時間 ms/tick 前回→今回 |
| --- | ---: | ---: |
| legacy | 1.067430099 | 27.163088 → 29.399362 |
| hyperpath | 1.001860114 | 30.181674 → 29.537545 |

StateGameLoopを含むスタックのsampled CPU合計差（親子inclusiveを加算した値ではない）:

| モード | 生の差 ms/tick | 補正後の差 ms/tick |
| --- | ---: | ---: |
| legacy | +1.332153 | -0.300620 |
| hyperpath | -0.619769 | -0.669512 |

計測条件と検証:

- 同じセーブ・設定・245アセットをSHA-256で確認。StateTicks 3,197,276,448 → 3,197,278,496、実tickサンプル各2,048。
- 前回と同じLegacy→Hyperpathの順。`--resume --precomputed --warm-precomputed --wait-background-ready`、Hyperpath索引ON、メインスレッドaffinity mask=1。
- 初期の準備中は停止し、背景処理完了までtickが進んでいないことを確認。追加の定常化・cold・全再構築なし。
- MSVC 19.44 x64 Release、`/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert ON、LTO OFF、静的CRT。前回と同じコンパイル・リンク設定。
- **WPR保存方式も同じモード同士で一致**。Legacyは前回・今回ともMemory、Hyperpathは前回・今回ともFile。Hyperpathは同じ1秒の停止中準備とreadyマーカーを入れてからbeginマーカーを記録した。
- CPUプロファイルのprovider・keywords・stacks・buffersを含む設定を、XML要素の出力順を除いて照合し、一致を確認した。WPAプロファイルとツールも同じ。
- 全763 nativeテスト・10,586,649 assertionsが通った。
- 両記録の開始・終了マーカーを確認し、kernelプロセス監査で競合ビルド・テスト・ゲームなし。lost events / buffersは0、製品関数の未解決シンボルなし。
- 生CSVのSHA-256、スタックとexclusiveの時間・サンプル数の保存則、支払い整合性を検証。WPAのInvalid Event警告がある場合、その時刻が測定マーカー外にあることも確認した。

前回と同じJP+ Multiple UnitsのNewGRF警告が出る入力。警告を抑制する変更はしていない。

- legacy: NewGRF警告 11,200 → 11,200件、今回の計測フックによるerror pause解除 1回。
- hyperpath: NewGRF警告 11,200 → 11,200件、今回の計測フックによるerror pause解除 1回。

ETLと対応exe/PDBは作業ツリー外にも保持し、場所・SHA-256を `recording-backup.json` に記録した。
固定入力は9ab0228dと同じ既存の検証済みZIPを参照し、重複ZIPは作っていない。

[全関数のinclusive/exclusive差分CSV](measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19/function-before-after.csv)、
[6関数の全数値](measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19/focus.json)、
[canonical照合](measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19/canonical-comparison.json)、
[検証結果](measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19/validation.json)。

補正の再計算:

```powershell
python -B utils/passenger_wpr_control.py docs/measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18 docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19
```
