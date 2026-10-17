# 67cbbf32: WPR の補正比較

`67cbbf32a48bac840d49037b2f9a90e8a1ca3aba` の製品ソースで、前回と同じセーブから
Legacy と Hyperpath（フィルタ ON）を各2,048 tick 計測した。
**f7c44f0d 比の補正後の差は Legacy −0.016887、Hyperpath +0.096556 CPU ms/tick。
MayUseHyperpathPolicy の inclusive は補正後0.423841で、前回0.426786から0.69%減だった。
この1ペアでは、tick 全体・対象関数の明確な改善は確認できない。**

終了 canonical は、両モードとも [f7c44f0d の同じモード](passenger-hyperpath-wpr-f7c44f0d-2026-10-17.md)
と24/24一致した。728テスト / 9,924,555 assertions もすべて成功した。
計測対象以外の製品コードは変更していない。

## 指定スクリプトによる補正

次のコマンドを、既定の分類・2,048 tick のまま実行した。
標準出力は [control.json](measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17/control.json)
にそのまま保存した。

```text
python -B utils/passenger_wpr_control.py docs/measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17 docs/measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17
```

以下は `StateGameLoop` がスタックにあるサンプルの CPU ms/tick。
旧・新の差は、それぞれ同じモード同士で取る。負の差は短縮、正の差は増加。
`machine_factor = 新 untouched / 旧 untouched`、補正後の新時間は `新 / machine_factor`。

| モード | f7c44f0d | 67cbbf32 生値 | 生の差 | machine_factor | 67cbbf32 補正後 | 補正後の差 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Legacy | 24.582154 | 24.974269 | +0.392115 | 1.016649608 | 24.565267 | **−0.016887** |
| Hyperpath | 26.199872 | 25.057370 | −1.142501 | 0.952881165 | 26.296427 | **+0.096556** |

Hyperpath の生値は1.1425 ms/tick 短くなったが、untouched も4.71%短い。
指定の補正ではその変動が取り除かれ、tick 全体の短縮は残らなかった。
Legacy の untouched は逆に1.66%長く、補正すると全体の差はほぼ0になった。
これは untouched を共通の速度変動の基準にした推定であり、周波数を直接測った結果ではない。

| 補正後の分類別差（CPU ms/tick） | Legacy | Hyperpath |
| --- | ---: | ---: |
| touched | +0.002842 | +0.147525 |
| heap（スクリプトの分類） | −0.019729 | −0.050969 |
| 合計 | −0.016887 | +0.096556 |

untouched の補正後差は係数の定義により0になる。
heap は ntdll の関数名に `Heap` / `Block` / `LowFrag` / `Rtlp` を含むスタック、
touched はそれ以外で既定の YAPF・passenger・cargo-list 等の語を含むスタック。
3分類は排他的で、親子の inclusive を足し合わせたものではない。

## MayUseHyperpathPolicy

Hyperpath の主スレッドにある
`PassengerJourneyLoadingContext::MayUseHyperpathPolicy` を新旧で照合した。
全スレッド・主スレッド・`StateGameLoop` 内の各範囲で、この関数の inclusive は一致する。
Legacy では両方ともサンプル0だった。

| Hyperpath（CPU ms/tick） | f7c44f0d | 67cbbf32 生値 | 67cbbf32 補正後 | 補正後の差 |
| --- | ---: | ---: | ---: | ---: |
| inclusive | **0.426786** | **0.403870** | **0.423841** | **−0.002944（−0.69%）** |
| exclusive（WPA Function 表） | 0.082032 | 0.081547 | 0.085579 | +0.003547 |
| exclusive（StateGameLoop スタック leaf） | 0.082032 | 0.080570 | 0.084554 | +0.002522 |

inclusive の生値の減少率は5.37%。補正には Hyperpath と同じ `0.952881165` を使った。
inclusive のサンプル数は874→827。exclusive は Function 表で168→167、
StateGameLoop スタックの leaf で168→165であり、命令位置の Function 表とスタック末端の帰属を区別する。
サンプル数は呼び出し回数ではなく、この小さい差から性能改善を断定しない。
incoming-carrier 等の各経路の実行回数は今回計数していないため、適用率はこの結果からは分からない。

[関数単体の生値・補正値 CSV](measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17/membership-before-after.csv)、
[今回 Hyperpath の全関数 inclusive / exclusive](measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17/hyperpath-functions.csv)、
[今回 Legacy の全関数](measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17/legacy-functions.csv)。

製品の simulation wall タイマーは Legacy 25.966598→26.362678、
Hyperpath 27.826782→26.132634 ms/tickだった。上の sampled CPU 補正とは別の測定値である。
既知の `StateGameLoop` スタックに入らないサンプルや外側の処理は、上の補正合計に含めていない。

## 状態・記録の検証

各モードとも開始3181119581、終了3181121629の2,048 StateTicksで正常終了。
終了 canonical は f7c44f0d の同じモードと **24/24一致、差分・欠落・追加0**。
支払い valid、packet・DB・所属・積載 digest、表の行数・論理 Work 等を含む。
64 bit digest は Python の整数で比較し、終了セーブの版情報によるバイナリ差とは区別した。

**Lost Events / Lost Buffers は両モードとも0**。製品関数の未解決表記0。
Function 表と Stack 表の総 Weight とサンプル数は一致した。
主スレッドのスタック不明は Legacy 723/59,091、Hyperpath 607/54,106で、
不明サンプルを特定の関数や `StateGameLoop` に推測で配分していない。
競合する計測プロセスの検出は両モードとも0。

WPA は各トレースで1件 Invalid Event を警告した。
時刻81.756524秒 / 76.564247秒は、集計終了60.052秒 / 54.947秒より後だった。
両 export は exit 0で、原文 CSV と警告ログを保存した。
NewGRF 警告と計測用の error pause 解除は前回と同じ条件で原文に残している。

## 条件と証跡

前回と同じ MSVC x64 Release、assert ON、LTO OFF、静的 CRT、`/Zi /Zo` と完全な対応 PDB。
Ryzen 9 9950X、Windows build 26200、主スレッド affinity mask 1。
precomputed ON、cold/warm、ロード済みバックグラウンド処理の完了待ち、途中の意図的な設定変更なし。
入力 SHA-256 は `306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
WPR のマーカーで集計区間を切り、終了集計とセーブは区間外に置いた。

チェックアウト `5e862d93f6` は、対象 `67cbbf32` と製品 src ツリーが一致し、
差分は補正スクリプトだけである（src ツリー `a1efff1664b667558bb54260657a4c9108ce643c`）。
計測用の複製 TU・PDB・ツール・比較元の SHA を保存した。
生 ETL（約2.38 / 1.97 GB）、対応 PDB と終了セーブは ignored の `build-wpr-67cbbf32/` に保持する。
Git には対象プロセスの WPA CSV を gzip で保存し、展開後の SHA が export 原文と一致することを検証した。

[再現手順](measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17/README.md)、
[集計](measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17/summary.json)、
[canonical 全48項目](measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17/canonical-comparison.csv)、
[ビルド・PDB 識別](measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17/build-identity.json)、
[検証結果](measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17/checks/validation.json)、
[証跡 SHA-256](measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17/evidence-sha256.json)。
