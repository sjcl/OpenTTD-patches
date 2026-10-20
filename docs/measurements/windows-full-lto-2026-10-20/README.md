# Full LTO + PGO 検証記録

- `verify.py`: 完了した既定 Release の検証と証跡保存。リポジトリルートで `python -B docs/measurements/windows-full-lto-2026-10-20/verify.py` を実行する。
- `verification.json`: compiler、PGO・実行ファイルの SHA-256、Full LTO フラグの確認件数、実 object の学習カウンター、CTest 結果。
- `compile-commands.json`: world tick と事前計算表の実コンパイルコマンド。
- `build-and-ctest.log.gz`: Full LTO ビルドと全 CTest のログ。

検証はローカル Windows x64。リモート Actions・x86・ARM64 の実行や新たな性能比較は含まない。
学習データの出自は [共通 PGO データの説明](../../../cmake/pgo/README.md) を参照。
