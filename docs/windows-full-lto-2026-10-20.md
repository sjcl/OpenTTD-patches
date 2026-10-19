# Windows 既定ビルドを Full LTO へ変更

Windows Clang の Release / MinSizeRel / RelWithDebInfo に指定する LTO を
`-flto=thin` から `-flto=full` へ変更した。本体の object library・実行ファイル・テストに適用する。
ローカルの `Build.ps1`、CMake preset、Windows CI・配布 workflow は共通の CMake 設定を使うため、同じ変更が適用される。

LLVM 23.1.3、LLD、Release の学習済み PGO データは継続して使用する。
`cmake/pgo/windows.profdata` は変更せず、CI での再学習も行わない。
以前の ThinLTO 比較・計測記録はその時点の条件として保持する。

Windows x64 の既定 Release を再ビルドし、CTest **784/784成功**（113.19秒）を確認した。
本体・テストの実コンパイルコマンド646件で `-flto=full` と PGO USE を確認し、ThinLTO・学習用フラグがないことを検証した。
実 object の IR にも PGO 情報が残り、`StateGameLoop` 4,096、`GameLoop` 21,239、
`Plan` 779,220、`PrepareGraph` 47,171 の学習カウンターを確認した。

検証スクリプトと結果は [検証記録](measurements/windows-full-lto-2026-10-20/README.md) に保存した。
リモート Actions・x86・ARM64 の実行はこの検証に含まない。
この変更では性能の再比較は行わない。
