# Clang ThinLTO + PGO Release

ThinLTO Release に PGO を適用した。同じ1,024 tickの WPR 計測では world CPU が **10.4%減**、事前計算表の構築 CPU が **8.0%減**だった。
775テスト・10,745,430 assertionsをすべて通過し、PGO 無効版とのマルチプレイも両方向で状態一致・desyncなしを確認した。

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

採用プロファイル SHA-256: `3efb8693f20a28440b1f6802b9fb08bb0da42d9cb16d5e9a4bf5ba55611f488d`。
IR プロファイルには 31,279 関数があり、world・経路探索・乗車判定の実行を確認した。
最終 `openttd.obj` の world 関数 entry count は 4,096。代表3 object に ProfileSummary と function entry count が入り、PGO 無効版にはないことを確認した。
最終版と無効版を `LLVM_PROFILE_FILE` 指定付きで実行しても raw profile を生成せず、最終版に学習用カウンターが残っていないことも確認した。

## 比較と正常性

PGO 無効の ThinLTO Release を同じソース・設定で新規ビルドし、同じ凍結入力と StateTicks `3197276448` → `3197277472` の1,024 tickで比較した。
表構築 wall は steady clock、CPU は GetProcessTimes の全 worker 合計。
CPU 計測と正常性の詳細を以下に示す。

各モード1回、同じ1,024 tickを WPR で収録した。表構築は tick 区間の前に測定した。ビルド・ユニットテスト・同期検証は性能計測と同時実行していない。

| 指標 | ThinLTO | ThinLTO + PGO | 増減 |
|---|---:|---:|---:|
| world CPU (ms/tick) | 22.1703 | 19.8566 | -10.44% |
| 旅客経路探索 CPU (ms/tick) | 0.4752 | 0.4826 | +1.56% |
| 乗車判定 CPU (ms/tick) | 0.8211 | 0.7895 | -3.85% |
| 経路探索 ∪ 乗車判定 CPU (ms/tick) | 1.0772 | 1.0377 | -3.67% |
| 表構築 CPU (s) | 63.5156 | 58.4375 | -8.00% |
| 表構築 wall (s) | 13.6406 | 12.6249 | -7.45% |

world は主スレッドの `StateGameLoop` を含むスタックの sampled CPU。経路探索・乗車判定はその部分集合で、対象関数の子孫を含む inclusive CPU。和集合では重複を一度だけ数えた。
対象関数一覧と生の集計を `summary.json` に保存した。PGO によるインライン化はスタックの帰属にも影響するため、個別項目の差は world 全体の差と合わせて読む。
両トレースとも lost buffers / lost events は0。各1回・単一マップの比較であり、学習区間と比較区間は重なる。別マップや GUI 描画への効果、統計的な有意差は判断していない。CPU affinity・電源設定は変更していない。

WPR を使用しない参考測定では world の粗い GetThreadTimes カウンターは 21.3470 → 19.2261 ms/tick、表構築 CPU は 66.3281 → 58.9219 s。15.625 ms刻みになるカウンターであり、WPR の値とは測定方法が異なる。
両計測で、同じ tick の終了状態24項目（支払い情報を含む）が一致し、前回の MSVC LTO / Clang ThinLTO 比較の終了状態とも一致した。
元セーブの NewGRF 警告は両 WPR 実行とも14,163行。計測ドライバーが開始前の error pause を各1回解除した。1,024 tick の測定区間内の追加解除は0回で、警告・解除情報は証跡に保存した。

### マルチプレイ

| サーバー | クライアント | 開始 tick | 終了 tick | 実行 tick 数 | 結果 |
|---|---|---:|---:|---:|---|
| baseline | pgo | 3197276448 | 3197276740 | 292 | 状態一致・desyncなし |
| pgo | baseline | 3197276448 | 3197276746 | 298 | 状態一致・desyncなし |

同一セーブをネイティブネットワーク経由で転送し、各4 worker・frame frequency 1で各256 tickを要求した。
2秒間隔の監視と停止伝播による超過を含む実行 tick 数を示した。各組の同じ終了 tick で比較し、方向間の異なる終了 tick の状態同士は比較していない。
接続直後の DB 再生は tick を進めず完了し、接続直後・走行後とも両 peer が一致した。これはこのセーブ・この区間での確認である。

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

最終実行ファイル: `build-clang-thin-pgo/pgo/Release/openttd.exe`。プロファイル: `build-clang-thin-pgo/profiles/real-map.profdata`。
[証跡](measurements/clang-thin-pgo-2026-10-19/README.md) にビルド指定、raw / indexed profile の識別情報、圧縮 `.profdata`、bitcode 検査、単体テスト・再構成テスト、tick 区間、状態・同期検証結果を保存した。
実行ファイル・PDB・raw profile・素材・セーブは ignored の build ディレクトリに保持する。
