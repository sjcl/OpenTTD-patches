# RelaxIncoming範囲分割の追加検証

旧構成は`5dc3b990ffadf8e134ba3318b1b03647356a4125`、採用コードは
`d7af532165489277745dafa23f7555ac9640be0d`。
測定時は前者のHEADにworking-tree候補を重ね、採用後に製品CPPだけを先にcommit・pushした。
`counted-source.json`/`timing-source.json`の生成元hashと、採用CPPのhashで対応を確認する。

`forward/`と`reverse/`は同一実行ファイルで全4,917行を完成させた通常順・逆順の比較。
`summary.json`は両方を含む平均で、`reference-warm-real`は共通の正解表を用意する準備として別扱い。
全timing標本をそのまま使い、外部負荷による補正や結果に応じた試行の除外はしていない。
カウンターなしのビルド条件は`timing-compile-flags.json`。
正確性・操作数は`all-plan/`と境界・合成テストのログで別に確認する。

`lambda-rejected/`と`array-rejected/`は速度が悪化した試作。
通常順4方式は完了しているが、棄却後に自分の逆順ベンチマークを停止したため、
その逆順は完了データとして使わない。`rejected.json`と通常ビルドへの復元記録を確認する。
`failed-counted-generation/`はSIMD生成時の宣言順不備の原文で、修正後に検証をやり直した。
`span-assembly.txt`は診断逆アセンブルから該当関数を抽出した派生資料。

`prefix-probe/`は上限512目的地の予備比較。スライス末尾まで進むため実際は519行。
`table_window`が指定上限と公開行数を記録する。全表の性能や採用判断には使わない。

`runtime-validation/`は採用コードを通常版・静的CRTでビルドした実ゲームの保持更新確認。
2,048tick後の全canonical項目を以前の同じ統合アルゴリズムと照合する。
`validation_only=true`で、通常tickの性能比較へ混入させない。
早期readbackはsimulationと表再生を進めず、derivedな公開行数・保持行数・Workを除いた保存状態を照合する。
それ自体は構築カーソルの完全再生を検証するものではない。

`method/`に実行手順、`timing-sources/`に計測用の生成TUを保存する。
測定に使った生成ツールは`method-sources/`に凍結し、公開用ツールの整理後も8生成ファイルが
byte単位で等しいことを`final-generator-equivalence.json`で確認した。
製品プロジェクトからこれらを外して通常ビルドへ戻し、共有ロックを解放したことも記録する。
バイナリ、セーブ、NewGRF資産は含めない。原文の改行をGitで変換せず、
`artifact-sha256.json`にmanifest自身を除く各ファイルのhashを記録する。
