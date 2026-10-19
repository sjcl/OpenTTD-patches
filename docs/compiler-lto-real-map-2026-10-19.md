# MSVC LTO と Clang ThinLTO の Release 実マップ比較

2026-10-19、`78c5cde779a5b0e758e32d435b31e6c1ff6e0436` の製品ソースを使い、MSVC LTO と Clang ThinLTO を比較した。
入力・計測フック・tick 区間は [LTO 無効時の比較](compiler-release-real-map-2026-10-19.md) と同じ。
この区間では Clang ThinLTO の world tick CPU が MSVC LTO より **13.4%少なかった**。各2回とも同じ傾向を確認した。
事前計算表の構築 wall は中央値で Clang 側が2.4%長く、試行の範囲は重なった。

## 条件

- Ryzen 9 9950X、16 core / 32 logical CPU、93.4 GiB RAM、Windows 11 Pro build 26200。
- MSVC 19.51.36260.0、clang-cl / LLD 19.1.5、x64 Release、`/O2 /Ob2 /MT`、assert OFF。
- 既存 MSVC 19.44.35227 と隔離取得した 19.44.35229 は LTCG 中に C1001 / LNK1000 で停止したため、Microsoft 公式 Stable チャネルの 19.51.36260 を SHA-256 検証後に作業フォルダーへ展開した。システムの Visual Studio は変更していない。
- MSVC のコンパイラ・librarian・リンカーを更新し、両コンパイラとも既存の MSVC 14.44 の STL・CRT・SDK・外部依存を使用。
- MSVC: **ゲーム本体の OBJECT library も含めて** `/GL`、実行ファイルのリンクに `/LTCG`。
- Clang: 同じ対象に `-flto=thin`、LLD の ThinLTO backend と専用キャッシュ。
- 外部依存ライブラリ・MSVC STL・CRT・素材・設定は前回と同じ。外部ライブラリは既存の native object であり、それらの再ビルドによる LTO は行っていない。
- 前回と同じ公開関数境界の PDB を使用し、両方 `/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO`。追加の `/Zi /Zo /Z7` は付けず、最適化・インライン化は有効。
- Clang の compiler-rt 追加リンクと、SDK `mt.exe` による元マニフェストの再埋め込みは前回と同じ。
- 製品ソースの追加変更はなく、前回のコピー TU と同じ計測処理を使用。今回のディレクトリに合わせて include パスだけ変更した。

指定セーブ `C:\Users\bebe\OneDrive\ドキュメント\OpenTTD\save\gc3833d807.sav` の SHA-256:
`916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。

通常 tick の計測は StateTicks `3197276448` → `3197277472` の **1,024 tick**。
MSVC → Clang → Clang → MSVC の順で各2回。ビルド・同期検査は性能計測と同時実行しない。
保存済み旅客 DB の再生完了を検証した後、tick を進めずに事前計算表を無効化・再有効化し、空からの全行構築を計測した。
構築の CPU は `GetProcessTimes()` の全スレッド user + kernel、wall は `steady_clock`。
通常 tick の CPU は WPR SampledProfile。主スレッドの `StateGameLoop` を含むスタックを world tick とし、経路探索・乗車判定は前回と同じ関数集合の inclusive 集計を使う。
両範囲の和集合では重複サンプルを1回だけ数える。
LTO によるインライン化で、内訳の帰属先は変わり得る。公開 PDB で残った関数境界に対する値であり、ソース上の処理領域を完全に復元した計測ではない。
通常 tick 全体の判断には world tick の CPU 時間を優先する。

MSVC 19.44 の失敗したビルドは性能試行に含めない。
追加デバッグ情報の撤回、コード生成1スレッド化、GUI TU の一時的除外、`/d2:-notypeopt` による試行も失敗し、最終バイナリではこれらの回避策を使用していない。
19.51 では本体の全 TU を再コンパイルし、librarian と linker も同じパッケージに統一した。
新 compiler の指定は MSBuild の `CLToolPath` / `LIBToolPath` / `LinkToolPath` で行ったため、CMakeCache の初期検出バージョンとは区別してビルドログ・ツールハッシュを保存した。

## 結果

各2回の中央値。通常 tick は CPU ms/tick、構築は秒。差は Clang / MSVC − 1。時間は小さい方がよい。

| 項目 | MSVC LTO | Clang ThinLTO | 差 |
|---|---:|---:|---:|
| world tick CPU（主スレッド） | 25.829 | 22.374 | −13.4% |
| 経路探索 CPU（公開関数境界） | 0.5553 | 0.4690 | −15.5% |
| 乗車判定 CPU（公開関数境界） | 0.8238 | 0.8029 | −2.5% |
| 経路探索・乗車判定の和集合 CPU | 1.1061 | 1.0552 | −4.6% |
| 事前計算表の構築 wall | 14.528 | 14.877 | +2.4% |
| 事前計算表の構築 CPU（全スレッド） | 67.273 | 69.516 | +3.3% |

| 試行 | world CPU ms/tick | 経路探索 CPU ms/tick | 乗車判定 CPU ms/tick | 構築 wall 秒 | 構築 CPU 秒 |
|---|---:|---:|---:|---:|---:|
| MSVC 1 | 25.8110 | 0.5572 | 0.8120 | 14.5908 | 67.1875 |
| Clang 1 | 22.4338 | 0.4623 | 0.8309 | 15.3124 | 71.4219 |
| Clang 2 | 22.3146 | 0.4757 | 0.7748 | 14.4425 | 67.6094 |
| MSVC 2 | 25.8469 | 0.5534 | 0.8355 | 14.4656 | 67.3594 |

world tick の範囲は MSVC 25.811–25.847、Clang 22.315–22.434 CPU ms/tick で重ならなかった。
主スレッド全体の CPU も MSVC 26.565、Clang 23.073 ms/tick と同じ傾向。
補助の `GetThreadTimes()` 集計は MSVC 25.635、Clang 22.415 ms/tick。これは15.625 ms単位の量子化があるため、細かい内訳には使っていない。
経路探索の関数範囲も2回とも Clang が低かったが、インライン化による帰属変更を含むため、経路処理そのものの削減量とは断定しない。
乗車判定・構築 wall の試行範囲は重なり、この小差だけで優劣を強く断定しない。

前回の LTO 無効時の world tick CPU と並べた参考値:

| コンパイラ | 前回 LTO 無効 ms/tick | 今回 LTO 有効 ms/tick | 差 |
|---|---:|---:|---:|
| MSVC（19.44 → 19.51） | 25.679 | 25.829 | +0.6% |
| Clang（両方19.1.5） | 26.378 | 22.374 | −15.2% |

異なる時刻の測定であり、MSVC はバージョン更新も含む。これを LTO 単独の因果効果とは扱わない。

## 正常性とマルチプレイ

性能4試行はすべて exit code 0、1,024個の連続した tick サンプルを生成した。
4試行相互、および前回の LTO 無効試行と、canonical inspector の24項目がすべて一致した。
終了 StateTicks は `3197277472`、公開済み4,936行、総 Work `1976379656`、貨物パケット468,937個、`payments_valid=true`。
packet、DB、ownership、loading、payment の digest と配送数等も一致した。
WPR 4本すべてで lost events / lost buffers は0。`StateGameLoop` の分類は各試行で主スレッド CPU の85%以上を占めることを検査した。

入力 NewGRF の容量変更警告は前回同様に各14,163行。
計測用コピーでロード時の error pause を1回解除し、通常 tick 区間中の追加解除は全試行0回だった。

同じ元セーブから loopback で異種コンパイラ接続を両方向に実行した。サーバー準備後、クライアントは製品のマップ受信・保存済み再生・参加処理を使用した。
性能計測終了後に独立したポート17750・17751で同時実行した。

| ホスト | クライアント | 要求 tick | 実進行 tick | 終了 StateTicks | 参加時・進行後の不一致 | network error / desync |
|---|---|---:|---:|---:|---:|---|
| MSVC LTO | Clang ThinLTO | 256 | 286 | 3197276734 | 0 / 0 | なし |
| Clang ThinLTO | MSVC LTO | 256 | 284 | 3197276732 | 0 / 0 | なし |

開始 StateTicks は両方向とも `3197276448`。
クライアントの再生 Work は両方向 `2804801877`、再生前後の StateTicks と frame は変化しなかった。
参加時の24項目は両方向間でも前回の LTO 無効時とも一致した。
進行後も、各方向の server / client 間の24項目はすべて一致し、packet・DB・ownership・loading・payment 等の digest と配送数を検証した。

性能計測の4試行は厳密に同じ1,024 tick区間である。同期検証の停止は2秒ごとの観測と将来 frame への pause 配布を使うため、実進行は要求256 tickを超え、両方向で終了 tickが異なった。
そのため、同期検証の終了状態については同じ tick に停止した各 server / client を比較し、終了 tick の異なる方向間・前回記録との直接比較は行わない。
ネットワークの既定 timeout は変更せず、観測ツールの上限だけ600秒とした。

## 記録

[測定記録](measurements/compiler-lto-real-map-2026-10-19/README.md) に、ビルド設定、実行ファイル・PDB のハッシュ、計測 JSON、WPA CSV、ログ、コピーした TU、再現ツールを保存した。
実行ファイル・PDB・ETL・セーブ・素材・ThinLTO キャッシュは ignored の `build-compiler-lto-comparison/` に保持する。
比較用実行ファイルは `build-compiler-lto-comparison/{msvc,clang}/Release/openttd.exe`。

前回の LTO 無効結果は別の実行時刻の参考記録であり、今回の2構成と4構成交互に測った結果ではない。
CPU affinity・電源設定は変更しておらず、ホストのクロック・他アプリによるばらつきが残る。
