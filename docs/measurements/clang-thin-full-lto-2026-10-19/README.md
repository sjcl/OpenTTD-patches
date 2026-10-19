# Clang ThinLTO / Full LTO の計測証跡

条件・結果・解釈は [比較レポート](../../clang-thin-full-lto-2026-10-19.md) を参照。

- `build-identity.json`: 入力・製品ソース・バイナリ・PDB・言語・コピー TU のハッシュ、ビルドフラグ、同一 revision、代表 LLVM bitcode object のハッシュとヘッダー。
- `builds/`: 生成した MSBuild 設定、コンパイルコマンド tlog、CMake overlay、構成・ビルドコマンド、生成した revision TU。
- `checks/`: 両構成の構成・ビルドログ。ビルドは同時実行したため、ログのビルド所要時間は速度比較として扱わない。
- `runs/`: 同じ1,024 tick の JSON・終了状態・WPR 欠落数・各関数の CPU 表、圧縮 WPA 原文 CSV・ログ。
- 有効試行は Thin 1回・Full 2回。`exclusions.json` と `checks/dropped-events-thin-2-*` に、WPR 欠落で不採用にした最後の Thin 試行の理由・ログ・実行メタデータを保存。
- `mp-thin-full/` と `mp-full-thin/`: 同じ元セーブからホスト・クライアントを入れ替えた同期検証。
- `copied-tus/`: 実際に使った計測 TU。両構成間では build 固有パスを正規化した内容が同一であることを検証。
- `tools/`: 当該実機で使ったツール。ROOT、LLVM、SDK、WPT、依存ライブラリ、前回の計測 TU 等の絶対パスを含む。再現時はパスと元セーブ・素材を用意する。

経路探索・乗車判定は inclusive なので単純加算せず、`route_or_boarding_ms` を使用する。
内部関数を解決するため両方 `/Z7` 付きで再ビルドした。インライン化により残る関数境界が変わり得るため、内訳だけで実処理の増減を断定しない。
バイナリ・セーブ・素材・ETL はリポジトリには含めない。
