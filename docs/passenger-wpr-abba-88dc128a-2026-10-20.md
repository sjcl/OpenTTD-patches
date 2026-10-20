# cdebb1a0 / 88dc128a Clang ThinLTO ABBA・validate計測

A=`cdebb1a029f84938956c0040082269dff745c44d`、B=`88dc128ad35f9a69a3db0673c301ca3f9031d8ee`。2026-10-20 JST、凍結済み`gc3833d807.sav`からLegacy／Hyperpathを各A→B→B→A、計8本、各2,048 tick採取した。Aの全treeは9439af0eと同一（`b8d78638dafb629eb0b8c38ab4ff3756243f3a5c`）。

**同じ版・同じモードの2本どうしでcanonical 24項目は4組すべて完全一致。A/B間の差は今回の仕様変更で予期されたもので、採用条件にしていない。両版の先行Hyperpath validateもmemo・lookupのviolations=0、lookup_duplicates=0だった。**

- Hyperpath `RoutePassengerJourneys` inclusive: 0.722120 → 0.885772（+22.66%） ms/tick。
- Legacy `RoutePassengerJourneys` inclusive: 1.005526 → 0.898759（-10.62%） ms/tick。
- Hyperpath generation packets: 23,417 → 5,154（-77.99%） / 2,048 tick。
- Legacy generation packets: 29,388 → 29,388（+0.00%） / 2,048 tick。

時間はメインスレッドの`StateGameLoop`を含むCPUサンプリングのinclusive。原因別のmicrosecondsだけは製品のpacket処理タイマーの合計で、WPRのinclusiveやスキップを含む走査全体の時間とは範囲が異なる。A/Bで処理対象が変わるため、同一処理量の命令単位の高速化とは呼ばない。

Hyperpathではgeneration処理の件数・原因別タイマーは減ったが、`RoutePassengerJourneys`のinclusiveはA 0.702091～0.742149、B 0.794897～0.976647 ms/tickと両方のB実行で高かった。今回の採取では、この関数の時間短縮は確認できない。

ただし、Hyperpathでは`RunTileLoop` inclusiveも8.374311→10.376622、`CallVehicleTicks`も9.198551→11.510443 ms/tickと広く増加している。LegacyのRouteはA/Bの範囲が重なる。全体の生CPU値も表に残すが、この各2本だけで差をrow-limited sweep単独の回帰・高速化やCPU周波数の変化と断定しない。機械状態とコード／処理経路の寄与はこの記録だけでは特定していない。

## Hyperpath

| 項目 | A 平均 [最小, 最大] | B 平均 [最小, 最大] | B−A | 変化 |
| --- | ---: | ---: | ---: | ---: |
| generation packets / 2048 tick | 23,417 [23,417, 23,417] | 5,154 [5,154, 5,154] | -18,263 | -77.99% |
| generation microseconds / 2048 tick | 234,800.0 [229,524.0, 240,076.0] | 73,230.0 [63,157.0, 83,303.0] | -161,570.0 | -68.81% |
| generation timer ms/tick | 0.114648 [0.112072, 0.117225] | 0.035757 [0.030838, 0.040675] | -0.078892 | -68.81% |
| generation timer us/packet | 10.026904 [9.801597, 10.252210] | 14.208382 [12.253977, 16.162786] | 4.181478 | +41.70% |
| sweeps full | 出力なし | 7,979 [7,979, 7,979] | 出力なし | — |
| sweeps row_limited | 出力なし | 33,736 [33,736, 33,736] | 出力なし | — |
| sweeps skipped_packets | 出力なし | 18,935 [18,935, 18,935] | 出力なし | — |
| RoutePassengerJourneys inclusive ms/tick | 0.722120 [0.702091, 0.742149] | 0.885772 [0.794897, 0.976647] | 0.163652 | +22.66% |
| RunPassengerWaitingEvents inclusive ms/tick | 0.701127 [0.681103, 0.721150] | 0.859154 [0.770965, 0.947343] | 0.158027 | +22.54% |
| StateGameLoop inclusive ms/tick | 18.769888 [18.390795, 19.148982] | 23.402140 [20.982296, 25.821984] | 4.632252 | +24.68% |
| tick wall ms/tick | 19.434467 [19.016018, 19.852917] | 24.328171 [21.649014, 27.007327] | 4.893704 | +25.18% |

## Legacy

| 項目 | A 平均 [最小, 最大] | B 平均 [最小, 最大] | B−A | 変化 |
| --- | ---: | ---: | ---: | ---: |
| generation packets / 2048 tick | 29,388 [29,388, 29,388] | 29,388 [29,388, 29,388] | 0 | +0.00% |
| generation microseconds / 2048 tick | 362,670.0 [316,136.0, 409,204.0] | 321,542.5 [314,139.0, 328,946.0] | -41,127.5 | -11.34% |
| generation timer ms/tick | 0.177085 [0.154363, 0.199807] | 0.157003 [0.153388, 0.160618] | -0.020082 | -11.34% |
| generation timer us/packet | 12.340751 [10.757316, 13.924187] | 10.941286 [10.689363, 11.193208] | -1.399466 | -11.34% |
| sweeps full | 出力なし | 39,153 [39,153, 39,153] | 出力なし | — |
| sweeps row_limited | 出力なし | 0 [0, 0] | 出力なし | — |
| sweeps skipped_packets | 出力なし | 0 [0, 0] | 出力なし | — |
| RoutePassengerJourneys inclusive ms/tick | 1.005526 [0.920210, 1.090842] | 0.898759 [0.876191, 0.921327] | -0.106767 | -10.62% |
| RunPassengerWaitingEvents inclusive ms/tick | 0.977457 [0.896770, 1.058144] | 0.875568 [0.857148, 0.893989] | -0.101889 | -10.42% |
| StateGameLoop inclusive ms/tick | 23.262750 [20.491561, 26.033940] | 20.942452 [20.518887, 21.366017] | -2.320299 | -9.97% |
| tick wall ms/tick | 24.257335 [21.174683, 27.339987] | 21.706282 [21.470233, 21.942330] | -2.551053 | -10.52% |

Aには`Passenger generation sweeps`の出力がない。A欄の「出力なし」は0を意味しない。Bの`full`・`row_limited`は全走査／公開行を限定した走査、`skipped_packets`は限定走査で処理を省略したpacketの観測件数。

## 各本の生値

| 順序 | 版 | mode | generation packets | generation us | sweeps full | row_limited | skipped_packets | Route inclusive ms/tick | Route samples | StateGameLoop ms/tick |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 01-A1/legacy | A | legacy | 29,388 | 409,204 | 出力なし | 出力なし | 出力なし | 1.090842 | 2,232 | 26.033940 |
| 01-A1/hyperpath | A | hyperpath | 23,417 | 240,076 | 出力なし | 出力なし | 出力なし | 0.702091 | 1,435 | 18.390795 |
| 02-B1/legacy | B | legacy | 29,388 | 328,946 | 39,153 | 0 | 0 | 0.921327 | 1,885 | 21.366017 |
| 02-B1/hyperpath | B | hyperpath | 5,154 | 63,157 | 7,979 | 33,736 | 18,935 | 0.794897 | 1,625 | 20.982296 |
| 03-B2/legacy | B | legacy | 29,388 | 314,139 | 39,153 | 0 | 0 | 0.876191 | 1,792 | 20.518887 |
| 03-B2/hyperpath | B | hyperpath | 5,154 | 83,303 | 7,979 | 33,736 | 18,935 | 0.976647 | 1,996 | 25.821984 |
| 04-A2/legacy | A | legacy | 29,388 | 316,136 | 出力なし | 出力なし | 出力なし | 0.920210 | 1,882 | 20.491561 |
| 04-A2/hyperpath | A | hyperpath | 23,417 | 229,524 | 出力なし | 出力なし | 出力なし | 0.742149 | 1,517 | 19.148982 |

各ラウンドはLegacy→Hyperpath、新しいプロセスで同じ入力をロードした。平均と実測範囲は各版2本の値で、信頼区間ではない。ABBAは直線的な時間ドリフトを釣り合わせるが、全ノイズを除去するものではない。untouched補正は行っていない。inclusiveは同じスタック内の同じ関数を1回だけ数え、親子を重複加算しない。全関数のinclusive/exclusiveとsamplesはCSVに保存した。

## 正しさとvalidate

- A: memo validated=13,679,678, conservative=12,071, violations=0, lookup_duplicates=0。lookup validated_groups=970,780, violations=0。validateと同じ版の通常indexのcanonical一致=True。
- B: memo validated=13,679,679, conservative=12,070, violations=0, lookup_duplicates=0。lookup validated_groups=970,769, violations=0。validateと同じ版の通常indexのcanonical一致=True。

- A/B間のlegacy canonical差: 0/24項目。差分を保存し、同じ版の2本で決定性を判定した。
- A/B間のhyperpath canonical差: 2/24項目。差分を保存し、同じ版の2本で決定性を判定した。

validateは両版ともWPR前に同じ入力から`--hyperpath-filter-mode validate --validation-only`で各1本。各2,048 tick・準備完了・準備中tick不変を確認し、性能平均には含めていない。`simulation_timing_valid=false`はvalidation-onlyの意図した値。Aはmerge後の7b3f2c4bを含む版として検証した。この1マップの成功は全入力の決定性を証明しない。

## 条件・採用検査・保管

- 入力46,620,248 bytes、SHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットを既存manifestに照合。StateTicks **3,197,276,448 → 3,197,278,496**。
- ClangCL/LLD 23.1.3、ThinLTO、assert OFF、PGO OFF、静的CRT、`/O2 /Ob2 /MT /Z7`、compiler-rt builtins、mt.exeでmanifestを再埋め込み。両版の正確なcheckoutからビルドし、exe/PDBを各々保存してから元ブランチへ戻した。計測フックはコピーTUだけ。
- merge後CMakeの既定は`-flto=full`だった。最初のconfigureの確認で検出し、採取前に`OPTION_NO_LTO=ON`で自動Full LTOを抑止、C/C++のRelease flagsへ`-flto=thin`を明示した。実clang-clコマンド・bitcode・builtins・対応PDB・抽出manifestを検証。製品CMakeの変更はしていない。
- 通常条件は`--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。追加warm-up tick・cold・再構築指定なし。
- A: 全818 nativeテスト、11,055,867 assertions成功。
- B: 全819 nativeテスト、11,055,928 assertions成功。
- 同じkernel CPU専用File profile、1ms、1,024×1,024KiB。短いpilot合格後、8本すべてevents/buffers欠落0・両マーカーあり。kernel process lifetimeの区間監査で競合ビルド／テスト／ゲームなし。関連チャットへ採取枠を調整した。
- WPA製品関数の未解決0、CSVのgzip展開ハッシュ、exclusive時間・samples保存則、支払い整合性を確認。InvalidEvent警告は測定マーカー外。
- ETL8本・終了セーブ10本・両版exe/PDBを作業ツリー外へ独立コピー。固定入力ZIPは既存の独立保管をハッシュ照合して再利用。[保管manifest](measurements/passenger-wpr-abba-88dc128a-2026-10-20/recording-backup.json)、[入力ZIP](measurements/passenger-wpr-abba-88dc128a-2026-10-20/input-backup.json)。

[各本CSV](measurements/passenger-wpr-abba-88dc128a-2026-10-20/focus-runs.csv)、[全関数A/B差CSV](measurements/passenger-wpr-abba-88dc128a-2026-10-20/function-before-after.csv)、[canonical差分](measurements/passenger-wpr-abba-88dc128a-2026-10-20/canonical-comparison.json)、[採用検査・validate](measurements/passenger-wpr-abba-88dc128a-2026-10-20/validation.json)、[比較JSON](measurements/passenger-wpr-abba-88dc128a-2026-10-20/comparison.json)、[再現レシピ](measurements/passenger-wpr-abba-88dc128a-2026-10-20/checks/)。各ラウンドに関数別inclusive/exclusive CSV、各モード配下に生WPA CSVのgzipを保存。checksは当日の絶対パスを含む再現資料で、その場所から実行する汎用ツールではない。
