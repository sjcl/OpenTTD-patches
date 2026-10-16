# 実マップの計測根拠

実装と判定は [本レポート](../../../passenger-precomputed-routing-2026-10-15.md) を参照。

- 最終製品コード：`22f235eea089076c0d0fd089d5b34857c42970d8`
- 比較旧版：`3f0c8aaac95058ec0b64040594c6a653275956e7` を `git archive` したソース
- `retention-cold-1` / `retention-warm-1`：最終版の通常構築 / 開始前全表構築
- `final-old-1` / `final-old-2`：補正機能を含まない旧版の繰返し
- `compact-*`：直前版 `abc802005e`。全表済みの2回は保存ファイルまで一致した。最終版も旅客・精算ハッシュが一致する。
- `summary.json`：最終版を優先した集計。`details.json`：個別標本からの詳細集計。
- `provenance.json` / `assets.json`：入力・コンパイラ条件・素材の来歴。実セーブと素材はGitへ追加していない。
- 各実行ディレクトリ：実行スクリプト、console、実tick標本、GetProcessTimes / OS peak、保存時のPacket・精算検査、圧縮した警告ログ。
- `ctest-retention.log`：最終通常ビルドで623/623成功。`tests-retention.log`：新方式17ケース・15,952アサーション。

`baseline-smoke-language` / `revision-probe` は元セーブの読み込み拒否を示す。原本のロード失敗後に作られたtitle画面の保存は実マップの結果ではない。`new-1024` / `fixed-smoke` / `final-new-1` は更新キューの収束待ち・メモリ拒否が残っていた旧実装で、最終版の性能として使用しない。`baseline-2048` / `new-2048-fixed` は1,255 tick地点のCargoDist計算待ちに関連する除外試行。

## 再現手順

通常製品ビルドは本レポートのCMakeコマンドを使用する。測定用は別のビルド先を使うか、測定後に再configureして `OPTION_PASSENGER_BENCHMARK=OFF` へ戻す。

1. 元セーブと必要素材を固定する。

   ```text
   python utils/passenger_precomputed_real_map.py freeze <save.sav> <OpenTTD-assets-directory> <build> <package>
   ```

2. それぞれのソースを同条件でconfigureする。Release、x64、assert ON、LTO OFF、`OPTION_PASSENGER_BENCHMARK=ON`。実行ファイルと、そのビルドに対応したlangをビルドルートへ出力する。

3. コピーした翻訳単位を計測用に切り替える。

   ```text
   python utils/passenger_precomputed_real_map.py prepare <source> <build> <source-commit>
   python utils/passenger_precomputed_real_map.py stdio <source> <build>
   cmake --build <build> --config Release --target openttd --parallel 8 -- /nodeReuse:false
   ```

   製品ソースは変更しない。Windows専用コンソールへのログ付け替えを計測用コピーで止め、ファイルへ記録する。全旅客配送は会社カウンタ、既知ODの実乗降は既存診断から読む。

4. 今回の入力はそのままでは両版がOD revision不整合として拒否した。`recover_counter <baseline-source> <temporary-build>` は一時リーダーにだけカウンタの引上げを加える。最大値UINT64_MAXは拒否する。今回の64→82以外の一般的なセーブ修復を検証したものではない。通常のafterload・再保存による変換は含まれる。0tick・`--resume`で作ったコピーを比較入力とし、回復機能を含まない別バイナリへ戻して比較する。

5. 同一入力で順番に動かす。例の引数は新しい空の出力ディレクトリを指定する。consoleのパス処理のため出力先に空白を含めない。

   ```text
   python utils/passenger_precomputed_real_map.py run <old-exe> <package> <old-output> --ticks 1024 --input <canonical.sav> --lang <old-build/lang>
   python utils/passenger_precomputed_real_map.py run <new-exe> <package> <cold-output> --ticks 1024 --precomputed --input <canonical.sav> --lang <new-build/lang>
   python utils/passenger_precomputed_real_map.py run <new-exe> <package> <warm-output> --ticks 1024 --precomputed --warm-precomputed --input <canonical.sav> --lang <new-build/lang>
   python utils/passenger_precomputed_real_map.py run <new-exe> <package> <reload-output> --ticks 0 --resume --input <warm-output/result.sav> --lang <new-build/lang>
   python utils/passenger_precomputed_real_map.py summarize <summary.json> <old-output> <cold-output> <warm-output> <reload-output>
   ```

`--warm-precomputed` は初回tick前に必須派生表を全構築する参照条件で、Optional Cache設定ではない。初期構築のCPUはプロセス総CPUへ含まれる。ゲームの通常フローは分割構築のまま。実tickは正確に照合し、ロード失敗、未完了、競合プロセスを成功扱いしない。NewGRF警告とerror pauseの計測用継続処理を残し、実際の容量・予約・乗降処理を迂回しない。

1024tickは同一経済期間内に収まる。会社の当期配送カウンタがリセットされる長い比較では、この差分をそのまま使用しない。全体のOS CPUとsimulation threadのelapsed、メモリの主要配列とOS peakを別の値として扱う。
