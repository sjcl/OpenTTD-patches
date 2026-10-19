# MSVC LTO / Clang ThinLTO の実マップ計測記録

比較内容・解釈・制限は [レポート](../../compiler-lto-real-map-2026-10-19.md) を参照。

- `build-identity.json`: 入力・製品ソース・実行ファイル・PDB・コピー TU のハッシュ、ビルドフラグ、隔離 MSVC 19.51 の公式パッケージ URL・SHA-256 と展開ファイルのハッシュ。
- `builds/`: 生成した CMake / MSBuild 設定、コンパイルコマンドの tlog。CMake の初期検出は既存 MSVC 19.44、実際の最終本体コンパイルとリンクは `CLToolPath` / `LIBToolPath` / `LinkToolPath` で指定した MSVC 19.51。
- `lto-object-checks.json`: 本体・事前計算・旅客 DB の代表 object のハッシュとヘッダー。Clang の LLVM bitcode と、MSVC の LTCG object を保持していたことを記録。
- `checks/`: 成功したビルドと、MSVC 19.44 の内部エラーなど除外した試行の圧縮ログ。
- `runs/`: 1024 tick の JSON、終了状態、WPR 欠落数、関数別 CPU、WPA 原文 CSV・ログ。トレース本体とバイナリは ignored の `build-compiler-lto-comparison/` に保持。
- `mp-msvc-clang/`、`mp-clang-msvc/`: 同じ元セーブから両方向で行ったマルチプレイ同期検証。
- `copied-tus/`: 測定バイナリに使用したコピー TU。製品ソースの変更はなく、前回のコピーから build 固有の include パスだけ変更。
- `tools/`: 当該実機で使ったツール。ROOT、SDK、LLVM、WPT、既存 STL・CRT・素材等の絶対パスを含む。再現時はパスを合わせ、前回の `build-compiler-comparison/` にある素材と測定 TU を保持する。`acquire_msvc.py` は取得した実行パッケージの SHA-256 を公式カタログと照合してから展開する。

`summary.json` の `sampled_cpu_ms` は inclusive である。経路探索と乗車判定を単純加算せず、重複を除いた `route_or_boarding_ms` を使う。
インライン化により残った公開関数境界が変わり得るため、内訳だけで LTO による実処理の増減を断定しない。
