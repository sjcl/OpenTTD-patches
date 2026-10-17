# Alternate モード1回の実マップ測定

対象: `594bbecad3bc4fb32a586d698ea8954d4f0a9eef`。
結果と解釈は [測定報告](../../passenger-hyperpath-filter-alternate-2026-10-17.md)。

MSVC 19.44 x64 Release、assert ON、LTO OFF、静的CRT。
通常のCMake再生成後、以下で既存の計測用コピーを準備・ビルドした。

```text
python -B utils/passenger_estimated_prepare.py SOURCE BUILD 594bbecad3 --cpu
python -B utils/passenger_hyperpath_build.py BUILD BUILD/build-alternate.log --target openttd openttd_test
```

前回と同様に、生成された `save-integrity/cargopacket_sl.cpp` の
`const auto &r = cp.passenger_journey;` の前に次の読み取りのみを追加し、
容量不足履歴もPacket digestに含める。製品の `src` は変更しない。

```cpp
const auto &failure = cp.passenger_boarding_failure;
Mix(merge, failure.service, failure.count, failure.since, failure.incarnation);
```

`openttd_test.exe --reporter compact` で通常の全727ケース / 9,899,874 assertionsが成功。
`checks/native-all.log` に原文を保存する。全CTestや隠しベンチマークの実行を意味しない。

`run.py BINARY PACKAGE LANG OUTPUT` でalternateを1回だけ実行する。
元のpackageは前回と同じ、入力SHA-256は
`306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
2,048 tick、中間1,024 tickで乗換コスト20→17の表更新。
初期/終了後の表構築とLinkGraph完了待ちは停止中、主スレッドCPU mask 1、
輸送追跡・乗車プロファイラは無効。`OPENTTD_` 環境のプローブ指定も子へ継承しない。
開始scriptに `hyperpath_filter_alternate` → `reset` → `unpause`、
終了scriptに `total` と保存・状態検査がある。

`analyze.py RUN OUTPUT_JSON` は圧縮consoleも直接読み、次を確認する。

- mode=alternate、ON/OFF双方の走査あり、完全なtick列とconsole合計の一致。
- 重複検出なし、準備完了、主スレッド固定、入力・tick範囲の一致。
- 前回の基準との中間/終了canonical各24項目、輸送・容量不足記録の一致。
- 全グループ数・対象走査数・通過候補数の保存。

両モードについて `us_per_walk = *_us / *_walks` を求め、
差に全walks/ticksを掛け、1,000で割ったものを節約ms/tickの推定とする。
`*_groups / *_walks` も出力して、割当先の平均グループ数を確認できる。
1プロセスの混在標本から、全走査が同じモードだった場合へ外挿する値。
`legacy_gap_reference_ms=5` はユーザー提示の概数で、今回Legacyを再測定していない。

製品の走査タイマーはmode選択・mask/occurrence準備の後から、
Hyperpathグループループ末尾まで。後続のキー統合・候補処理は含まない。
counterと時計取得の観測コストを含む。全tickの内訳を測るプロファイルではない。
単発試行で、コンテキスト単位の信頼区間は推定していない。

`freeze.py BUILD PACKAGE` で元packageの資産hash、入力、実行ファイルと言語を検査し、
生のtick列、console原文gzip、MSVCのビルド/テストログ、使用したコピーTUを保存する。
セーブ・実行ファイル・ゲーム資産そのものは含めない。
`evidence-sha256.json` はmanifest自身以外の保存byte hash。
元の版との保存ファイル比較にはリビジョンメタデータの違いも入るため、
ゲーム状態の比較はcanonical各項目で行い、ファイルhashは別に記録する。
