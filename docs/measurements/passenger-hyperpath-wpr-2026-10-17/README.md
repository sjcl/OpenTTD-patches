# Legacy / Hyperpath CPU サンプリングの再現条件

対象ブランチは `merge/hyperpath-with-table-build`、取得時の HEAD は
`cdd7513528c96565677c92f68d3d0cb8caa98294`。
製品 src のツリーは `cb3e0ee59208f17636cbe8a48cd8a7f4660121de`。
製品コードの性能変更は加えず、コピーした計測 TU に開始・終了の待機だけを加える。

同一バイナリ、同一セーブから Legacy（`passenger_journey_hyperpath=false`）と
Hyperpath（同 true、`hyperpath_filter_on`）を各2,048 StateTicks 実行する。
両方とも precomputed / cold-precomputed / warm-precomputed / wait-background-ready。
途中の表更新は入れず、その他の共通設定は実行した `game_start.scr` に記録する。
主スレッドだけ CPU affinity mask 1 に固定する。ワーカーの並列性は維持する。

入力 SHA-256:
`306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
StateTicks は `3181119581..3181121629`。
実マップの既存 NewGRF 容量警告と、計測 TU による error pause 解除はログに残す。

## ビルド

MSVC x64 Release、assert ON、LTO OFF、静的 CRT。
Release C++ フラグは `/O2 /Ob2 /DNDEBUG /Zi /Zo`、リンクは
`/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO`。
`utils/passenger_estimated_prepare.py SOURCE BUILD cdd7513528 --cpu` の後、
`utils/passenger_wpr_prepare.py BUILD` を実行してビルドする。
再準備する場合は先に CMake を再生成する。
後者は容量不足履歴も Packet の保存状態ダイジェストに含める。

`openttd.exe` と対応する PDB の SHA-256、RSDS、ビルドログの識別は
`build-identity.json`、ネイティブテスト結果は `checks/native-all.log`。
コピーした計測ソースは `copied-tus/` に gzip で残す。

## 記録

`utils/passenger_wpr_record.ps1 -Build BUILD` だけを UAC で管理者実行する。
新規の `BUILD/legacy` と `BUILD/hyperpath` を使用する。
ゲームと Python runner は通常権限で、既存の計測排他と競合監視を使う。
runner の共通引数:

```text
run BINARY FROZEN_PACKAGE BUILD/MODE --ticks 2048 --timeout 1800
--measurement-wait 120 --precomputed --hyperpath-filter
--cold-precomputed --warm-precomputed --wait-background-ready
--main-thread-affinity-mask 1 --wpr-profile --lang BUILD/lang
```

Legacy は `--no-hyperpath`、Hyperpath は `--hyperpath` を追加する。
読み込み・初期表構築・ロード済みバックグラウンドジョブの完了待ち・診断 reset の後に
`wpr-ready.txt` を書き、管理者 helper が `wpr -start CPU` を実行する。
開始マーカーを書いてからゲームを再開する。
2,048 tick 完了後はゲームを待機させ、終了マーカーと `wpr -stop cpu.etl` を実行する。
ETL 保存後に診断・状態の読み取りとセーブを行う。
helper は自分で開始に成功した記録だけを停止する。

## WPA 書き出し

Microsoft の公式 ADK から Windows Performance Toolkit を作業フォルダ内へ展開した。
WPAExporter は `11.7.395.48728`。プロファイルは公式 catalog の CPU Usage (Sampled)
列定義から `utils/passenger_wpr_profile.py` で生成した `cpu-sampled.wpaProfile`。
ゲームプロセスに絞ったスタックと leaf 関数の表を使用する。

```text
wpaexporter -i BUILD/MODE/cpu.etl -symbols -marks MODE-begin MODE-end
  -profile cpu-sampled.wpaProfile -outputfolder BUILD/MODE/wpa
```

`_NT_SYMBOL_PATH` に対応するビルドの Release ディレクトリと Microsoft symbol server、
`_NT_SYMCACHE_PATH` に作業フォルダ内のキャッシュを設定する。
[WPAExporter の公式説明](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/exporter)。

今回の最終書き出しは、初回の記号解決で作ったキャッシュを使い、
`-symcacheonly` を追加した。出力先は両モードとも `wpa-v3`。
初回のセッション関連付けエラーと、折り畳まれた表だけの出力を修正し、
`SessionIndices` と `InitialExpansionQuery=[Series Depth]:<=1024` を指定した。
使用したプロファイルと最終コマンドはこのフォルダに保存したものを参照する。

```text
python -B utils/passenger_wpr_export.py BUILD TOOLKIT --output-name wpa-v3 --cached-symbols-only
python -B utils/passenger_wpr_summarize.py BUILD EVIDENCE --export-name wpa-v3
```

シンボルキャッシュが無い場合は先に `--cached-symbols-only` を外して記号解決する。
export は新規の出力先名を使う。集計は原文 CSV の process/thread 小計行を除き、
各サンプルの Weight と Count を足し、WPA の Function 表と Stack 表の総量を照合する。
`*-functions.csv` は全体/主スレッド/StateGameLoop の scope、関数名、
inclusive/exclusive の ms・サンプル数・ms/tick。
`function-diff.csv` は両モードの ms/tick と差を持つ。
inclusive は親子で重複するので足し合わせない。
`allocation-callers.csv` は StateGameLoop 内の RtlAllocateHeap/RtlFreeHeap ごとに、
最も近い名前付きの非 STL アプリケーションフレームを集計する。
最適化・インライン展開があるため、これは確保した個別オブジェクトの識別ではない。

記録区間中に自動発生する GameLoop の表準備も主スレッド全体には含める。
初期の停止中 warm の除外だけをもって、StateGameLoop 外の準備が0とは仮定しない。
詳細な結果と範囲の違いは [報告書](../../passenger-hyperpath-wpr-2026-10-17.md) を参照。

生 ETL、PDB、セーブ、ゲーム資産はローカルの ignored build ディレクトリに保管する。
Git にはゲームプロセスの CSV、集計、再現用プロファイル、診断と識別情報だけを含める。
