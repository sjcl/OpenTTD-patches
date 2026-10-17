# Alternate モードによる Hyperpath 走査の実マップ測定

`merge/hyperpath-with-table-build` をpullし、
`594bbecad3bc4fb32a586d698ea8954d4f0a9eef` をMSVCでビルドした。
製品 `src` のツリーは `cb3e0ee59208f17636cbe8a48cd8a7f4660121de`。
製品コードを追加変更せず、alternateを1プロセスで1回測定した。

**OFF 37.515 → ON 12.748 µs/走査の差から、全走査をONにした場合の
節約量は約1.735 ms/tickと推定された。1 ms/tickを上回る。**
中間・終了のcanonical各24項目と輸送数は前回の基準に一致し、実マップも正常終了した。

## 走査時間

終了時の製品診断の原文:

```text
Passenger hyperpath walk time: filtered_us=915200; filtered_groups=11268680; unfiltered_us=2690668; unfiltered_groups=11563981
```

filter行は `mode=alternate`。`enabled=false` は従来の純ON判定の表示で、
実際に両モードが走ったことは以下のwalksで確認する。

| 実際のモード | walks | 時間合計 µs | 訪問groups（省略分含む） | µs/走査 | groups/走査 |
| --- | ---: | ---: | ---: | ---: | ---: |
| ON / filtered | 71,789 | 915,200 | 11,268,680 | 12.748471 | 156.969 |
| OFF / unfiltered | 71,723 | 2,690,668 | 11,563,981 | 37.514716 | 161.231 |

両モードを合わせて143,512走査 / 2,048 tick = **70.074219走査/tick**。
したがって指定の計算式では、

```text
(37.514716339 − 12.748471214) × 70.07421875 / 1000
= 1.735475279 ms/tick
```

全走査が各モードだった場合の計時区間コストは、
OFF **2.628814**、ON **0.893339 ms/tick**の外挿値。
実際の混在実行で計時した区間の合計は **1.760678 ms/tick**。
ON側の平均groups/走査はOFFより約2.6%少なく、上の推定にその補正は加えていない。

今回の点推定は「1 ms/tick未満」という条件に該当しない。
約5 msのLegacy差という概数に対しては約34.7%相当の大きさであり、
走査区間に差があることを確認できた。
このA/Bだけでは、残る走査コスト・前後処理・tick全体の内訳までは確定しない。

参考として、混在実行のsimulation平均 / p95 / p99は
**29.581 / 42.696 / 57.463 ms/tick**、Journey平均は4.049 ms/tick。
通常前半のsimulation平均は28.749、表更新中の後半は30.413 ms/tick。
これらは混在実行全体の観測値で、純ON/OFFのtick全体差の測定には使用しない。

## 動作の照合

全groupsは `13,160,739 checked + 9,671,922 skipped = 22,832,661`、
通過候補は589,281で、前回の純モード実行の総groups・対象walks・通過候補数と一致。
中間・終了とも、旅客Packet（容量不足履歴を含む）、DB、所属、積載/予約、
支払い、表の進行と論理Workを含むcanonical各24項目がすべて一致した。

`Schedule transport` の乗車289,835・乗換133,611・配送127,877人、
`Untimed transport` の乗車4・乗換202・配送102人、
容量不足記録75,731 Packet / 138,499人分の機会も一致。
これらの累積機会数は一意の旅客数とは解釈しない。
版情報も保存されるため、版をまたぐ状態比較はcanonical項目で行い、
終了セーブのSHA-256は集計JSONに別途記録した。

準備完了、全tick列とconsole合計、主スレッド固定を検査し、
監視対象の別OpenTTD・ビルド・テストの重複検出は0。
NewGRF警告は21,165行、測定用のerror pause解除は1回で、原文に保持した。

## 方法

[前回](passenger-hyperpath-filter-2026-10-17.md) と同じ回収済み実マップ、
入力SHA-256 `306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
StateTicks `3181119581..3181121629` の2,048 tick。
前半1,024 tickは通常進行、直後に乗換コスト20→17の表更新を開始する。
初期/終了後の表構築と読み込み済みLinkGraphの完了待ちは停止中に済ませ、
通常tickの時間に含めない。

```text
passenger_journey_stats hyperpath_filter_alternate
passenger_journey_stats reset
… 2,048 tick進行 …
passenger_journey_stats total
```

MSVC 19.44 x64 Release、assert ON、LTO OFF、静的CRT、主スレッドCPU mask 1。
輸送追跡・乗車プロファイラは無効。
計時はpullした製品の `Passenger hyperpath walk time` を使い、
既存のコピーTUで正確なtick数を駆動して状態を読み取る。
入力の既存NewGRF容量変更警告と測定用のerror pause解除も原文ログに残す。

全体の走査頻度を `R = (filtered_walks + unfiltered_walks) / ticks` とし、
次の式で全走査がONだった場合の節約ms/tickを推定する。

```text
F = filtered_us / filtered_walks
U = unfiltered_us / unfiltered_walks
節約ms/tick = (U - F) × R / 1000
```

ON/OFFは積み込みコンテキスト単位の割当で、同一コンテキスト内のメモ化を保つ。
1回の混在標本からの外挿であり、コンテキスト単位の信頼区間は計算していない。
各モードのグループ数/走査も併記して割当先の重さを確認する。
タイマーの範囲はmode・mask/occurrence準備の後からHyperpathグループループ末尾まで。
後続のキー統合・候補処理や、tick全体の内訳は含まない。
約5 msというLegacyとの差はユーザー提示の概数で、今回Legacyを再測定していない。

## MSVC検証と証跡

`openttd_test.exe --reporter compact` が **727ケース / 9,899,874 assertions** 成功。
ON・OFF・alternateの等価性と、グループキー/Packet到着系統のassertもこのビルドで通る。
[原文テストログ](measurements/passenger-hyperpath-filter-alternate-2026-10-17/checks/native-all.log)。

[機械可読の集計](measurements/passenger-hyperpath-filter-alternate-2026-10-17/summary.json)、
[再現手順・計算式](measurements/passenger-hyperpath-filter-alternate-2026-10-17/README.md)、
[ソースとバイナリの識別](measurements/passenger-hyperpath-filter-alternate-2026-10-17/source-identity.json)、
[証跡のSHA-256](measurements/passenger-hyperpath-filter-alternate-2026-10-17/evidence-sha256.json)。
生tick列・console gzip・MSVCログ・使用したコピーTUを保存する。
セーブ・実行ファイル・ゲーム資産そのものはGitに含めない。
