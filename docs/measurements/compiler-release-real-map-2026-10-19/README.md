# Release コンパイラ比較の記録

[比較レポート](../../compiler-release-real-map-2026-10-19.md)。

- `summary.json`: 各試行の CPU・構築時間、中央値、24項目の状態一致検査。
- `build-identity.json`: 入力・ビルド・計測 TU のハッシュとコンパイラ設定。
- `runs/`: 同じ1,024 tick区間の生記録、WPA の圧縮 CSV、欠落イベント検査、関数ごとの inclusive CPU。
- `mp-*/`: Clang/MSVC 間の loopback 接続検査。
- `copied-tus/`: 製品ソースを置換せずにビルドした計測専用コピー。
- `tools/`: この実機のパスを含む再現用ツールと小さな整数演算検査。

ETL・PDB・ゲームバイナリ・セーブ・素材は `build-compiler-comparison/` に保持し、この記録には収録しない。
