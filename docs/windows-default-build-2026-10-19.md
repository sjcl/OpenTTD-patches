# Windows の既定ビルド: LLVM 23 + ThinLTO + 共通 PGO

Windows の既定 Release を Clang/LLVM 23.1.3、LLD、ThinLTO、用意済み PGO データへ変更した。
本機の通常インストールも LLVM 18.1.8 から23.1.3へ更新し、winget の版固定を23.1.3で復元した。
Visual Studio 同梱の19.1.5ではなく、`C:/Program Files/LLVM` を選択する。

`os/windows/Build.ps1`、`windows-release` preset、Windows CI・配布 workflow は同じ設定を使う。
本体 object library・実行ファイル・テストに `-flto=thin` を明示する。
Visual Studio の IPO プロパティだけでは実際の ThinLTO が有効にならなかったため、実コマンドと bitcode で確認した。
設定方法は [COMPILING.md](../COMPILING.md)、データの説明と手動更新方法は [PGO README](../cmake/pgo/README.md) を参照。

## 学習データ

指定の実セーブのコピーで StateTicks `3197276448` → `3197280544` の4,096 world tickを実行し、正常終了したデータを採用した。
入力 SHA-256 は `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。
共通データ `cmake/pgo/windows.profdata` の SHA-256 は `1bcd4ad1bc15c07b26ae52cde1b9b2318675e1278d07204678eb252d476c4bf3`。
元セーブ・素材・raw profile は配布しない。CI では学習せず、このデータを一度読み込んで最終版をビルドする。

IR のローカル関数名を basename に揃え、`-fmacro-prefix-map` でソースルートも `/openttd` に揃えた。
MSVC ABI の無名名前空間もファイルパスのハッシュを含むため、この両方が必要だった。
既存データ内の9種類の識別子は LLVM と同じ xxh3 の下位32 bitで共通パスの値へ変換し、間接呼び出し先の参照も再索引した。
以後の手動学習では同じ compiler flag により共通識別子を直接生成する。
無名名前空間の仕様は [LLVM の実装](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/AST/MicrosoftMangle.cpp#L485-L516) に基づく。

学習用 driver/console フック、後述の修正で変化した Windows API 取得関数・ラッパー、同名ファイル間で識別子が衝突する初期化関数の307レコードを除外した。
学習元の revision・ソース fingerprint・割当器のハッシュ・変換内容は `cmake/pgo/windows.json` に記録した。
単一の Windows x64 実マップで用意したデータを x86/x64/ARM64 で共有する。別マップ・別アーキテクチャの性能向上率は測定していない。

## 終了時の修正

最新構成の実マップ検証で、main-thread TLS のブロックキャッシュ破棄後にグローバル YAPF キャッシュがそこへ書き戻す不具合を確認した。
破棄済みキャッシュには再投入せず解放するよう修正し、通常時のブロック再利用は維持した。
終了時まで生存する割当器の回帰テストも追加した。

個別テストの終了時には、worker が初めて `Kernel32.dll` の loader を初期化して `atexit` 登録を待ち、
CRT 終了ロックを保持した main thread が同じ worker の終了を待つデッドロックも確認した。
既に常駐する Kernel32 の handle を借りて関数を取得し、遅いタイミングでの終了処理登録を不要にした。

## 検証

- 本機 Windows x64: 最終 Release の CTest **784/784成功**（113.77秒）。単体テスト・本体ロード・4種類のスクリプト回帰検証を含む。
- ブロック再利用・終了時の個別検証: 2 case、24,683 assertions成功、プロセス終了も成功。
- 最終版には学習用フラグ・raw profile の出力がないことを確認。
- 別配置先の実 `openttd.cpp` で world の実行回数4,096、`GameLoop` 21,239を確認。
- 別配置先の実 `passenger_precomputed.cpp` で `PrepareGraph` 47,171、`Plan` 779,220を確認。
- 同じ実ソースの無名名前空間関数の測定済み0カウンター、および独立した無名名前空間・間接呼び出し fixture の正の30カウンターを確認。hot な内部関数は最適化でインライン化されるため区別して記録した。
- `actionlint` と PowerShell parse に成功。既存の無効化済み署名ステップに対する constant-false 指摘だけを lint の対象外にした。

Actions は構文検証までで、リモートの x86/ARM64 ジョブはこの作業では実行していない。
ARM64 配布は `windows-11-arm` で native LLVM を使用する。
以前の LLVM 19 の性能比較結果は履歴として残しており、この LLVM 23 構成の改善率として扱っていない。

最終ローカル実行ファイルは `build-windows-default/release/openttd.exe`。
ビルド条件・ハッシュ・検証ログは [証跡](measurements/windows-default-build-2026-10-19/README.md) に保存した。
