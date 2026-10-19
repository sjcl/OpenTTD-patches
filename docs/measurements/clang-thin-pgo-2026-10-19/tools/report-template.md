# Clang ThinLTO + PGO Release

<!-- OUTCOME -->

## ビルドと学習

ベースは `246bbdbf8f0b59e524d40b7b0b104f5a965c7858`。ランタイムの製品コードは変更せず、CMake に Release 用の PGO 切替を追加した。
Clang/LLD/llvm-profdata 19.1.5、x64、`/O2 /Ob2 /MT /Z7`、assert OFF、`-flto=thin` を維持した。
計測機は AMD Ryzen 9 9950X（16 core / 32 logical CPU）、93.4 GiB RAM、Windows 11 Pro build 26200。
本体 OBJECT library と実行ファイルの両方に ThinLTO と PGO の指定を適用する。
外部依存ライブラリは前回と同じ native の静的ライブラリで、PGO 用には再構築していない。

[Clang 19 の手順](https://releases.llvm.org/19.1.0/tools/clang/docs/UsersManual.html#profiling-with-instrumentation)に従い、IR instrumentation PGO を使用した。

1. `OPTION_PGO=GENERATE` で `-fprofile-generate -fprofile-update=atomic` を付けた学習用 Release をビルド。
2. 指定セーブ `C:\Users\bebe\OneDrive\ドキュメント\OpenTTD\save\gc3833d807.sav` の凍結コピーを使用し、保存済み DB の再生、空からの表構築、通常4,096 tick、結果の保存を実行。
3. `LLVM_PROFILE_FILE` で出力先と `%p-%m` を指定し、同じ LLVM の `llvm-profdata merge` で indexed `.profdata` を作成。
4. `OPTION_PGO=USE` と `PGO_PROFILE_FILE` で `-fprofile-use` を付けた最終 Release を別ディレクトリにビルド。

入力 SHA-256 は `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。
学習区間は StateTicks `3197276448` → `3197280544`。
3構成で同じパスの計測用コピー TU を使用し、static 関数のプロファイル対応も保った。強制 noinline は使用していない。
学習用バイナリの実行時間は性能比較に使用しない。
null 描画のシミュレーション向けに単一実マップで学習しており、GUI 描画や別マップの性能は未評価。
比較区間は学習区間内に含まれるため、未知入力に対する性能評価ではない。

<!-- PROFILE -->

## 比較と正常性

PGO 無効の ThinLTO Release を同じソース・設定で新規ビルドし、同じ凍結入力と StateTicks `3197276448` → `3197277472` の1,024 tickで比較した。
表構築 wall は steady clock、CPU は GetProcessTimes の全 worker 合計。
CPU 計測と正常性の詳細を以下に示す。

<!-- RESULTS -->

<!-- MULTIPLAYER -->

## CMake の使い方

既存の Clang ThinLTO のコンパイラ・依存ライブラリ・Release 設定を維持し、構成時に次を追加する。
PGO の既定値は `OFF` で、Debug 等の構成には PGO フラグを追加しない。

```text
学習用: -DOPTION_PGO=GENERATE
適用版: -DOPTION_PGO=USE -DPGO_PROFILE_FILE=C:/absolute/path/real-map.profdata
無効化: -DOPTION_PGO=OFF
```

学習後は通常終了させて `.profraw` を書き出し、同じ LLVM バージョンで merge する。プロファイル生成用の実行ファイルを配布版として使わない。
ソースやコンパイラを変更したら再学習する。今回の configure / build / train コマンドは証跡に保存した。
Windows の ClangCL と CMake 4.0 では `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON` の初期値を使うと、OBJECT library も含む対象に CMake の `-flto=thin` を適用できる。
今回の測定では前回と同じ明示的 overlay を使い、`openttd_lib` と `openttd` の自動 IPO は無効にして `-flto=thin` を直接指定した。

`USE` はプロファイルを SHA-256 ごとの build 内スナップショットにコピーして使用する。
元ファイルの変更を CMake の構成依存に登録し、同じパスの内容を更新してもコンパイルフラグが変わることで再ビルドする。
clang-cl の `GENERATE` では、コンパイラに対応する profile runtime の検索パスも設定する。分割ビルドのテスト実行ファイルにも必要な指定を適用する。

## 成果物と証跡

<!-- ARTIFACTS -->
