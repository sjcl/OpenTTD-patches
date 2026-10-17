# f7c44f0d の WPR 再測定

対象は `f7c44f0d1aa55e32145b00b5e60ab0754eafe8f4`、
製品 src のツリーは `919700f79dbad480290d639efba49a413fbd2270`。
比較元は [前回の WPR 計測](../passenger-hyperpath-wpr-2026-10-17/README.md)
（記録を保存した commit `3e0930523af8ada2c2d98ab8a41311362bf23ba1`）。
対象には `f62f5ba3e3` の診断コピー削減と `f7c44f0d` のブロック再利用が含まれる。

MSVC x64 Release、assert ON、LTO OFF、静的 CRT、`/O2 /Ob2 /DNDEBUG /Zi /Zo`、
リンク `/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO`。
独立した `build-wpr-f7c44f0d` にビルドし、以前の ETL・PDB は保持した。
`passenger_estimated_prepare.py SOURCE BUILD f7c44f0d --cpu`、
`passenger_wpr_prepare.py BUILD` で同じ計測 TU を準備する。
実際に使用した TU とビルドの識別は `copied-tus/` と `build-identity.json`。

同じ凍結セーブから Legacy と Hyperpath（フィルタ ON）を順に各2,048 tick 実行する。
開始 StateTicks は3181119581、終了は3181121629。
入力 SHA-256 は `306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
両モードとも precomputed / cold-precomputed / warm-precomputed / wait-background-ready、
主スレッド affinity mask 1、途中の意図的な設定変更は入れない。
実行引数・WPR のゲートとマーカー・WPA のプロファイルは前回と同じ。

今回の WPAExporter は `11.7.395.48728`。
前回の Windows symcache を新 build の `symcache/` にコピーし、ゲーム PDB は除いた。
xperf による新しいゲーム symcache の事前準備は `0xc0000005` で異常終了したため、
WPA 本体でローカルの新しい PDB を読み込んで最終 CSV を作成した。
失敗した事前準備のコマンド・ログは `checks/`、成功した最終 export は各モードの
`export-command.json` と `wpa-export.log` に記録する。

```text
python -B utils/passenger_wpr_export.py BUILD TOOLKIT --output-name wpa --local-symbols-only
```

`--local-symbols-only` は `_NT_SYMBOL_PATH` をビルドの Release ディレクトリに限定する。
Windows の既存 symcache は使い、新しいゲームの symcache は対応するローカル PDB から生成する。
Windows symcache がない場合は、前回の方法で先に Microsoft の記号を解決する。

`passenger_wpr_summarize.py BUILD EVIDENCE --export-name wpa` が通常の関数別 CSV を作る。
その後、次のコマンドで同じモード同士の canonical と YAPF の比較を行う。

```text
python -B utils/passenger_wpr_compare.py BEFORE_EVIDENCE AFTER_EVIDENCE
```

canonical は `save-integrity.json` の24項目すべてを Python の整数精度で比較する。
変更版と旧版の終了セーブの SHA を同一性の判定には使わない。

YAPF の範囲は、主スレッドのスタックに `openttd.exe!` のフレームがあり、
その関数名に `Yapf` を含むもの。入口関数、テンプレート、コンテナの破棄を含む。
inclusive は同一スタック中の関数を1回だけ数え、exclusive はスタックの leaf。
`yapf-before-after.csv` に全関数を出し、`comparison.json` に対象2関数の数値を抜粋する。
`canonical-comparison.csv` は各モード24項目を全件出力する。
`tick-branches-before-after.csv` は StateGameLoop 直下等の処理の新旧比較で、
YAPF の部分的な減少と tick 全体の差を区別するために残す。
ヒープ時間は RtlAllocateHeap / RtlFreeHeap のいずれかがあるサンプルの和集合で、
親子の inclusive を重複加算しない。時間は Weight / 2,048 の CPU ms/tick。
サンプル数は関数の呼び出し回数ではない。

WPA の原文 CSV は各モードのサブフォルダに gzip で保存する。
新旧の生 ETL・PDB・セーブと資産は ignored build ディレクトリに保持する。
