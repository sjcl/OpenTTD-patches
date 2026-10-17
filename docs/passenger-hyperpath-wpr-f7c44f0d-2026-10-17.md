# f7c44f0d: YAPF ヒープ処理の WPR 再測定

`f7c44f0d1aa55e32145b00b5e60ab0754eafe8f4` で、前回と同じセーブから
Legacy と Hyperpath（フィルタ ON）を各2,048 tick 計測した。
**YAPF 配下の RtlpFindEntry は約92～94%、RtlpInsertFreeBlock は約95～97%減少した。
終了 canonical は、両モードとも前回の同じモードと24項目すべて一致した。**

比較元は [前回の WPR 結果](passenger-hyperpath-wpr-2026-10-17.md)
（証跡 commit `3e0930523a`、実行 src は `cb3e0ee59208f17636cbe8a48cd8a7f4660121de`）。
対象には `f7c44f0d` の大きいブロック再利用と、直前の `f62f5ba3e3` の診断コピー削減が含まれる。
今回は計測・集計以外の製品コードを変更していない。

## YAPF 配下の対象2関数

単位は sampled CPU ms/tick。inclusive は各スタックで1回だけ加算、exclusive は leaf。
主スレッドのうち、製品フレームに `Yapf` を含むスタックを同じ条件で抽出した。

| モード・関数 | 前回 inclusive | f7c44f0d inclusive | 減少率 | 前回 exclusive | f7c44f0d exclusive |
| --- | ---: | ---: | ---: | ---: | ---: |
| Legacy / RtlpFindEntry | 1.805231 | 0.113782 | 93.70% | 1.797911 | 0.113295 |
| Legacy / RtlpInsertFreeBlock | 0.341783 | 0.016109 | 95.29% | 0.340316 | 0.016109 |
| Hyperpath / RtlpFindEntry | 1.794930 | 0.143605 | 92.00% | 1.789072 | 0.142140 |
| Hyperpath / RtlpInsertFreeBlock | 0.651326 | 0.021482 | 96.70% | 0.650837 | 0.021482 |

inclusive のサンプル数は順に Legacy が3,697→233、700→33、
Hyperpath が3,676→294、1,334→44。
exclusive でも同様に減少している。

| 範囲 | Legacy 前回→今回 | Hyperpath 前回→今回 |
| --- | ---: | ---: |
| YAPF 全体 CPU ms/tick | 2.963856 → 0.703104 | 3.408614 → 0.748551 |
| YAPF 配下のヒープ CPU ms/tick | 2.498078 → 0.169436 | 2.924225 → 0.203171 |
| ヒープ時間の差 | −2.328642 | −2.721054 |

ヒープの行は RtlAllocateHeap / RtlFreeHeap のいずれかがあるサンプルの和集合。
親子の inclusive を足して二重計上していない。
サンプル数を呼び出し回数やキャッシュヒット回数とは解釈しない。

[YAPF 全関数の新旧 inclusive/exclusive CSV](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/yapf-before-after.csv)、
[対象関数と集計 JSON](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/comparison.json)。

## 終了 canonical

Legacy は前回 Legacy、Hyperpath は前回 Hyperpath と比較した。
**各24/24一致、差分・欠落・追加項目とも0**。
Packet 状態（容量不足履歴を含む）、DB、所属、積載、支払い、表の行数・論理 Work、
輸送集計等を含む。64 bit digest を浮動小数点に変換せず照合した。

両方とも開始3181119581、終了3181121629の2,048 StateTicksで、正常終了。
支払い状態も valid。
版情報を含む終了セーブのバイナリ SHA が異なることを、canonical 不一致とは扱っていない。

[全48件の照合 CSV](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/canonical-comparison.csv)、
[Legacy 終了状態](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/legacy/save-integrity.json)、
[Hyperpath 終了状態](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/hyperpath/save-integrity.json)。

## tick 全体との区別

対象ヒープの改善量を、そのまま tick 全体の改善量にはしない。
製品の simulation wall タイマーの平均は次の通り。

| モード | 前回 ms/tick | 今回 ms/tick | 差 |
| --- | ---: | ---: | ---: |
| Legacy | 26.060275 | 25.966598 | −0.093677 |
| Hyperpath | 28.433514 | 27.826782 | −0.606732 |

主スレッド全体の sampled CPU は Legacy 28.485565→28.418196、
Hyperpath 28.551788→27.999294 ms/tick。
StateGameLoop 直下の CallVehicleTicks は −1.235 / −1.844 ms/tickだった一方、
RunTileLoop は **+1.301 / +1.050 ms/tick** 増えている（順に Legacy / Hyperpath）。
これらを含む他のコスト増で、YAPF 部分の減少が相殺されている。
RunTileLoop 等の増加原因は今回確定していない。
同じセーブ・設定・主スレッド固定での1ペアであり、周波数やキャッシュ状態の影響も分離していない。

[tick 処理別の新旧比較](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/tick-branches-before-after.csv)、
[今回 Legacy の全関数](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/legacy-functions.csv)、
[今回 Hyperpath の全関数](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/hyperpath-functions.csv)。

## 条件と証跡

前回と同じ MSVC x64 Release、assert ON、LTO OFF、静的 CRT、`/Zi /Zo` と対応 PDB。
Ryzen 9 9950X、Windows build 26200、主スレッド affinity mask 1。
precomputed ON、cold/warm、ロード済みバックグラウンド処理の完了待ち、
途中の意図的な設定変更なし。Legacy は hyperpath OFF、Hyperpath は ON / filter ON。
入力 SHA-256 は `306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
実際のコマンド・スクリプト・識別情報を各フォルダに保存した。

WPR の開始・終了マーカーで区間を指定し、終了集計とセーブは区間外。
**Lost Events / Lost Buffers は両方0**。
WPA の Function 表と Stack 表の総 Weight・サンプル数は両モードの全スレッド/主スレッドで一致した。
ゲーム本体 Function の未解決表記は0。
主スレッドのスタック n/a は Legacy 789/58,187、Hyperpath 695/57,327で、
不明なスタックを YAPF に推測で配分していない。

WPA は各トレースで1件 Invalid Event を警告したが、時刻80.805秒 / 80.168秒は、
集計終了マーカー59.139秒 / 58.602秒より後だった。
事前の xperf シンボル準備は異常終了したため、最終書き出しは WPAExporter に
ローカルの新しいゲーム PDB と前回の Windows symcache を渡して行った。
最終の両 export は exit 0、原文 CSV と警告ログを残した。

新しい allocator ケースを含む **728テスト / 9,924,555 assertions** 成功。
計測監視対象の別ゲーム・ビルド・テストとの競合検出は0。
NewGRF 警告と計測用の error pause 解除は前回と同じ条件で原文ログに残す。

[集計](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/summary.json)、
[再現条件](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/README.md)、
[ビルド・PDB・計測ソースの識別](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/build-identity.json)、
[証跡 SHA-256](measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/evidence-sha256.json)。
生 ETL（約2.49 / 2.75 GB）、PDB と終了セーブは ignored の `build-wpr-f7c44f0d/` に保持した。
