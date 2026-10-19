# Clang/LLVM と MSVC の Release 実マップ比較

2026-10-19、`a5aa133cfe51cd39ab9175396f318824af97b30e` に Clang 用の整数演算分岐修正を加えて測定した。
このマップでは Clang による全体の明確な高速化は確認できなかった。
経路探索の関数範囲では CPU 時間が約14%減ったが、乗車判定との和集合はほぼ同等だった。
world tick 全体は中央値で約2.7%増加した。Clang の試行間のばらつきが大きく、この小差から全体の優劣を断定しない。

## 入力とビルド

入力はユーザー指定の `C:\Users\bebe\OneDrive\ドキュメント\OpenTTD\save\gc3833d807.sav`。
元ファイルをコピーし、全試行で同じコピーから開始した。
SHA-256 は `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。
元セーブとユーザーの設定は変更していない。

- AMD Ryzen 9 9950X、16 core / 32 logical CPU、物理メモリ93.4 GiB、Windows 11 Pro build 26200。
- MSVC 19.44.35227.0 と clang-cl / LLD 19.1.5。Clang は MSVC ABI / STL と LLVM compiler-rt を使用。
- 両方 x64 Release、`/O2 /Ob2`、C++20、静的 CRT `/MT`、assert OFF、LTO OFF。
- PNG、LZMA、LZO、ZSTD、Opus 等は同じ既存の x64-windows-static ライブラリをリンク。
- 同じ言語ファイル、同じ凍結素材、同じ最小設定。描画・音声・音楽は null ドライバ。
- 旅客経路設定は保存値を継承。事前計算のローカル Worker 設定は両方とも既定の4。

ビルドには次の互換対応が必要だった。

1. clang-cl が `_MSC_VER` を定義していても `_udiv128` を提供しないため、`PassengerWide::Ceil()` では既存の `__uint128_t` 分岐を選ぶ。
2. Clang のリンクに `clang_rt.builtins-x86_64.lib` を追加する。
3. LLD が生成した実行マニフェストの `level` 属性が不正だったため、リンク後に Windows SDK の `mt.exe` で `os/windows/openttd.manifest` を埋め直す。

36組の積・切り上げ除算と、飽和境界の小さな検査を両コンパイラで実行し、正常終了と同一 digest を確認した。
製品 C++ の変更は整数演算のコンパイラ判定1行だけ。
計測はコピーした TU を CMake の追加 include から差し替え、元のシミュレーション・経路選択・Work 境界を保持した。
実行ファイル・PDB・コピーした TU のハッシュとフラグは [build-identity.json](measurements/compiler-release-real-map-2026-10-19/build-identity.json) に保存した。

## 計測方法

全試行で StateTicks `3197276448` → `3197277472` の1,024 tickを進めた。
順序は MSVC → Clang → Clang → MSVC。各2回の中央値を使う。

保存済み旅客 DB の再生を検証付き `StepLoad()` で完了させ、tick を進めずに事前計算を無効化→再有効化した。
そこから全行完成までを構築時間として測り、診断をリセットして通常 tick を開始した。
この手順で保存済み再生と空からの表構築を分けた。
構築の CPU 時間は Windows `GetProcessTimes()` による全スレッドの user + kernel 合計、wall は `steady_clock`。

通常 tick は WPR の SampledProfile を同期ゲートの間だけ記録した。
WPAExporter の begin/end マーカー指定で同じ区間を切り出し、主スレッドの `StateGameLoop` スタックを world tick とした。
関数名は対応する PDB から解決した公開関数境界を使う。
インライン展開された内部処理は残った呼出境界に含まれる。ソース行単位の内訳ではない。

経路探索は `PassengerServiceDatabase::PlanPrecomputed/PlanInternal`、`PassengerPrecomputedTable::Plan`、
`PassengerJourneyPlanner::Plan/PlanOnboard`、`PassengerSchedulePlanner::Plan` を含むスタック。
乗車判定は `PassengerJourneyLoadingContext::CanBoard/MayUseHyperpathPolicy` を含むスタック。
いずれも inclusive。両範囲が重なるサンプルは和集合では1回だけ数える。
乗車候補の列挙など、これらの関数の外側の費用は world tick に含まれる。
全プロセススレッド・主スレッド全体の値も JSON に別記した。

各 ETL の lost events / lost buffers は0。
`GetThreadTimes()` の補助計測も残したが、この環境では15.625 ms単位のため、小さい旅客処理の CPU 計測には採用しなかった。

## 結果

時間は小さい方がよい。通常 tick は CPU ms/tick、構築は秒。差は Clang / MSVC − 1。

| 項目 | MSVC 中央値 | Clang 中央値 | 差 |
|---|---:|---:|---:|
| world tick CPU（主スレッド） | 25.679 | 26.378 | +2.7% |
| 経路探索 CPU | 0.5493 | 0.4735 | −13.8% |
| 乗車判定 CPU | 0.8179 | 0.8513 | +4.1% |
| 経路探索・乗車判定の和集合 CPU | 1.1065 | 1.1039 | −0.2% |
| 事前計算表の構築 wall | 15.635 | 15.907 | +1.7% |
| 事前計算表の構築 CPU（全スレッド） | 73.125 | 74.102 | +1.3% |

| 試行 | world CPU ms/tick | 経路探索 CPU ms/tick | 乗車判定 CPU ms/tick | 構築 wall 秒 | 構築 CPU 秒 |
|---|---:|---:|---:|---:|---:|
| MSVC 1 | 25.8597 | 0.5644 | 0.8213 | 16.1272 | 74.9219 |
| Clang 1 | 24.4267 | 0.4528 | 0.7866 | 15.5189 | 72.2188 |
| Clang 2 | 28.3292 | 0.4942 | 0.9160 | 16.2953 | 75.9844 |
| MSVC 2 | 25.4981 | 0.5342 | 0.8144 | 15.1421 | 71.3281 |

経路探索単体の減少は2回とも見られた。
world tick、乗車判定、構築は試行の範囲が重なっている。
CPU affinity や電源設定は変更しておらず、ホストのクロック・他アプリの影響は残る。
旅客処理全体を関数名の和集合で集計した参考値は MSVC 3.3284、Clang 3.5050 CPU ms/tick。
この参考分類と経路探索・乗車判定の範囲は [summary.json](measurements/compiler-release-real-map-2026-10-19/summary.json) と集計スクリプトで明示した。

## 正常性とマルチプレイ

4試行すべて正常終了し、1,024個の tick サンプルと終了セーブを生成した。
24項目の canonical inspector 出力は全試行で一致した。
旅客・貨物パケットの状態、所有先、乗車・積み降ろし状態、支払い、旅客 DB、公開行数、総 Work、配送数などを比較した。
支払い検証は全試行 true。終了時は公開済み4,936行、総 Work `1976379656`。

同じ元セーブで loopback の異種コンパイラ接続を両方向に実行した。
ホストの保存済み再生を準備スクリプトで完了させ、クライアントは製品のマップ受信・再生・参加処理を使った。
ホスト準備中とクライアント再生中に StateTicks の進行がないことも検査した。
両方向は性能計測終了後に独立したポートで同時実行した。

| ホスト | クライアント | 要求 tick | 実進行 tick | 参加時の不一致 | 進行後の不一致 | network error / desync |
|---|---|---:|---:|---:|---:|---|
| MSVC | Clang | 256 | 288 | 0 | 0 | なし |
| Clang | MSVC | 256 | 288 | 0 | 0 | なし |

両方向とも開始 StateTicks は `3197276448`、終了は `3197276736`。
pause の配布・観測待ちを含むため要求256 tickに対して実進行は288 tickとなり、終了地点は全 peer で一致した。
公開済み行は2,074→2,121、保持行は2,862→2,815で一致し、途中の表構築を含む輸送を検証した。
packet、DB、ownership、loading、payment の digest、配送数、総 Work など24項目は参加時・終了時とも一致した。
クライアント再生 Work は両方向 `2804801877`、再生中の StateTicks は変化しなかった。

ネットワークの既定値（`max_join_time=500` tick、`max_join_time_total=900`秒等）を維持した。
計測ツール側の観測上限だけ600秒に設定した。
初回のホスト準備待ち試行は300秒の観測上限に達して参加前に終了したため、成功数には含めない。
最終試行のクライアント参加観測は両方向とも約203秒で完了した。

入力 NewGRF の JP+ Multiple Units から容量変更に関する警告が出た。
性能4試行では各14,163行で一致した。ロード時の error pause を計測用コピーで1回解除し、tick 区間中の追加解除は0回。
警告を含むこの実マップの条件での比較である。

## 記録と再現

詳細な [測定記録](measurements/compiler-release-real-map-2026-10-19/README.md) に、JSON、各関数の CPU 表、圧縮した WPA 原文 CSV、コンソール、コピーした TU、ツールを保存した。
ETL・実行ファイル・PDB・セーブ・素材は ignored の `build-compiler-comparison/` に保持する。
計測フック付き Release の実行ファイルは `build-compiler-comparison/msvc/Release/openttd.exe` と
`build-compiler-comparison/clang/Release/openttd.exe`。

この実機の主なビルド指定は次のとおり。

```text
-G "Visual Studio 17 2022" -A x64
-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
-DOPTION_USE_ASSERTS=OFF -DOPTION_NO_LTO=ON
-T "ClangCL,host=x64"  # Clang側のみ
```

Clang の追加リンク指定は `CMAKE_EXE_LINKER_FLAGS` に以下を設定する。

```text
/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO
/DEFAULTLIB:"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/lib/clang/19/lib/windows/clang_rt.builtins-x86_64.lib"
```

MSVC は同じ debug/opt/incremental 指定を使い、追加の compiler-rt は使わない。
Clang のリンク後に実行したマニフェスト処理:

```powershell
mt.exe -manifest os/windows/openttd.manifest '-outputresource:BUILD/Release/openttd.exe;#1'
```

計測時の設定・CLI・WPR プロファイル・実行順は保存した `tools/` と各 `process.json` に含まれる。
ツールはこの実機の絶対パスを記録しているため、別環境では ROOT、SDK、LLVM、素材のパスを合わせる。
