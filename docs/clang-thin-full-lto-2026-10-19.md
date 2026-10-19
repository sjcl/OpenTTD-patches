# Clang ThinLTO とフル LTO の Release 実マップ比較

2026-10-19、同じ Clang/LLD 19.1.5 で `-flto=thin` と `-flto` を比較した。
製品ソースは `258af9a456400997b4d24e1a981e5f178d8b5cdd`。前回以降、製品コードの変更はない。
world tick CPU は Full が中央値で0.88%少なかったが、Full の2回の幅（21.9597〜22.4372 ms/tick）がモード間の差を上回った。
Thin の有効試行も1回であり、この測定から Full LTO の明確な性能優位は判断できない。表構築時間もほぼ同じだった。

## 条件と計測方法

- AMD Ryzen 9 9950X、16 core / 32 logical CPU、93.4 GiB RAM、Windows 11 Pro build 26200。
- 両方とも新規 x64 Release ビルド、`/O2 /Ob2 /MT`、C++20、assert OFF。
- ゲーム本体の `openttd_lib` と実行ファイルの両方に、ThinLTO は `-flto=thin`、Full LTO は **`-flto`** を適用。CMake の自動 IPO 指定は両対象で無効にし、指定の混在を避けた。
- 同じ MSVC 14.44 STL・CRT・SDK、同じ native の外部依存ライブラリと compiler-rt、同じ素材と設定を使用。外部ライブラリを LTO 用に再ビルドしていない。
- 両方 `/Z7 /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO`。内部化された関数も解決できる同じデバッグ情報を付与した Release。ThinLTO のみ専用 backend キャッシュを使用。
- 前回と同じ計測用コピー TU を使い、build 固有の include パスだけ変更。製品コードの変更・関数の強制 noinline はない。
- LLD 由来のマニフェスト問題を避けるため、両方ともリンク後に SDK `mt.exe` で元の `os/windows/openttd.manifest` を埋め込んだ。

指定入力は `C:\Users\bebe\OneDrive\ドキュメント\OpenTTD\save\gc3833d807.sav`。
SHA-256 は `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。
同じ凍結コピーから、StateTicks **`3197276448` → `3197277472` の1,024 tick** を各2回実行した。
順序は Thin → Full → Full → Thin。ただし最後の Thin は WPR が **1,449,719 events** を欠落したため、性能集計から除外した。
再試行の管理者起動は Windows 側で取り消されたため代替値を作らず、有効な **Thin 1回・Full 2回**を採用した。ビルドや同期検証を性能計測と同時実行していない。

保存済み旅客 DB の再生を検証付きで完了させてから、tick を進めずに事前計算表を無効化・再有効化し、空からの全行構築を計測した。
構築 wall は `steady_clock`、CPU は `GetProcessTimes()` の全スレッド user + kernel 合計。
通常 tick の CPU は同期ゲート間の WPR SampledProfile を begin/end マーカーで切り出した。
主スレッドの `StateGameLoop` を含むスタックを world tick とし、経路探索・乗車判定は前回と同じ関数集合の inclusive 集計を使った。
両範囲の重複は和集合では1回だけ数える。
LTO モードによるインライン化で内訳の帰属先が変わり得るため、内訳はスタック上に残った関数境界に対する参考値であり、全体の判断には world tick CPU を優先する。
初回の追加 `/Z7` なしの試行では、Full LTO の内部関数名を解決できず集計検証が失敗したため採用せず、両構成を `/Z7` 付きで再ビルドして全試行を取り直した。

## 結果

有効試行は ThinLTO 1回、Full LTO 2回。以下は各モードの中央値（Thin は単一値）。CPU は小さいほど軽く、増減は Full / Thin − 1。

| 指標 | ThinLTO | Full LTO | Full の増減 |
|---|---:|---:|---:|
| world tick 全体 CPU (ms/tick) | 22.3948 | 22.1985 | -0.88% |
| 旅客経路探索 CPU (ms/tick) | 0.4789 | 0.5003 | +4.47% |
| 乗車判定 CPU (ms/tick) | 0.8235 | 0.7974 | -3.17% |
| 経路探索 ∪ 乗車判定 CPU (ms/tick) | 1.0854 | 1.0770 | -0.77% |
| 事前計算表の構築 CPU (s) | 65.9375 | 65.7969 | -0.21% |
| 事前計算表の構築 wall (s) | 14.0543 | 14.0536 | -0.01% |

| 試行順 | world CPU (ms/tick) | 経路探索 CPU (ms/tick) | 乗車判定 CPU (ms/tick) | 構築 CPU (s) | 構築 wall (s) |
|---|---:|---:|---:|---:|---:|
| wpr-thin-1 | 22.3948 | 0.4789 | 0.8235 | 65.9375 | 14.0543 |
| wpr-full-1 | 22.4372 | 0.5184 | 0.8263 | 65.9062 | 14.0579 |
| wpr-full-2 | 21.9597 | 0.4821 | 0.7684 | 65.6875 | 14.0493 |

経路探索と乗車判定は重複する inclusive 範囲であり、和集合を別記した。
表構築は全 worker の CPU 合計であり、wall 時間より大きくなる。

## 正常性とマルチプレイ

採用した3試行すべて終了コード0、連続する1,024 tickを確認。終了時の24項目はすべて一致し、支払い情報も正常。前回の MSVC LTO / Clang ThinLTO 比較の終了状態とも一致した。
WPR の lost buffers / lost events は採用した3試行とも0。

| サーバー | クライアント | 開始 tick | 終了 tick | 実行 tick 数 | 結果 |
|---|---|---:|---:|---:|---|
| thin | full | 3197276448 | 3197276712 | 264 | 接続・状態一致、desync なし |
| full | thin | 3197276448 | 3197276708 | 260 | 接続・状態一致、desync なし |

同一セーブをネイティブのネットワーク経由で転送し、サーバー・クライアント各4 worker、frame frequency 1で確認した。
各方向256 tickを要求し、2秒間隔の監視と停止伝播による超過を含む実測区間を上表に示した。各組の同一終了 tick で状態を比較し、方向間で終了 tick が異なる場合はその終了状態同士を比較していない。
接続直後の保存済み DB 再生は tick を進めず完了し、接続直後・走行後ともサーバーとクライアントの状態が一致した。これはこのセーブ・この短い区間での検証である。

入力 NewGRF 由来の警告は各試行14163行で同一。準備時のエラーポーズ解除は各1回、計測 tick 内の追加解除は0回だった。

## 記録

[計測証跡](measurements/clang-thin-full-lto-2026-10-19/README.md) に、ビルド指定、実行ファイル・PDB・LLVM bitcode object・コピー TU のハッシュ、各試行の JSON、圧縮 WPA CSV・ログ、同期検証結果、再現ツールを保存した。
実行ファイル・PDB・ETL・素材・セーブ・ThinLTO キャッシュは ignored の `build-clang-lto-comparison/` に保持する。
比較バイナリは `build-clang-lto-comparison/{thin,full}/Release/openttd.exe`。
CPU affinity・電源設定は変更しておらず、ホストのクロック・他アプリによるばらつきが残る。結果はこの実マップ・区間・環境に対するもの。
