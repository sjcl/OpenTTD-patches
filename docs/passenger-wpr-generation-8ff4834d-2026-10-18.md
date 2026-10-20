# 8ff4834d: 通常条件のLegacy / Hyperpath WPR

`8ff4834dba34f6ae18ea2068a542ecf47a4ac5e5` で、各モードの定常入力から通常条件を各2,048 tick、1回ずつ記録した。
比較対象は前回の88cb61e3通常条件（candidate）。終了canonicalは保存したが、一致判定には使っていない。

**Legacyの期限超過の滞留は解消していない（overdue=87,035、oldest_ticks=588）。**
generation処理回数はLegacyがほぼ不変、Hyperpathが2.55%減だった。

`cause=generation` の packets は処理回数で、同じパケットの再処理を含む。時間は診断カウンターの実時間の合計。
WPRのinclusive CPU時間とは計測方法が異なる。各ms/tickは2,048で割った値。

| モード | generation処理回数 前回 → 今回 | 診断時間合計 ms 前回 → 今回 | 診断 ms/tick 前回 → 今回 | 処理回数 / 時間の変化 |
| --- | ---: | ---: | ---: | ---: |
| legacy | 15,776,304 → 15,778,297 | 27351.315000 → 30720.418000 | 13.355134 → 15.000204 | +0.01% / +12.32% |
| hyperpath | 5,978,149 → 5,825,854 | 3622.378000 → 4352.659000 | 1.768739 → 2.125322 | -2.55% / +20.16% |

`RunPassengerWaitingEvents` はCPU Usage (Sampled)の `StateGameLoop` を含むスタックが対象。
各スタック内で同じ関数を1回だけ数えたinclusiveで、親子のinclusiveは加算しない。

| モード | 前回 CPU ms/tick | 今回 CPU ms/tick | 変化 | 今回inclusive samples |
| --- | ---: | ---: | ---: | ---: |
| legacy | 15.314484 | 16.226173 | +5.95% | 33,231 |
| hyperpath | 2.958418 | 3.655130 | +23.55% | 7,486 |

終了時の処理待ち:

| モード | pending 前回 → 今回 | overdue 前回 → 今回 | oldest_ticks 前回 → 今回 | maximum_station_backlog 前回 → 今回 |
| --- | ---: | ---: | ---: | ---: |
| legacy | 87,000 → 87,035 | 87,000 → 87,035 | 2,576 → 588 | 3,580 → 3,564 |
| hyperpath | 116,611 → 116,391 | 6,215 → 5,954 | 42 → 42 | 3,677 → 3,351 |

**Legacyは終了時にも overdue=87,035、oldest_ticks=588 が残り、期限超過の滞留は解消していない。**
pendingには将来のイベントも含まれる。上の表は終了時点の値であり、区間内の最大overdueは測っていない。

tick全体の実時間（計測対象のシミュレーションのみ）:

| モード | 前回 ms/tick | 今回 ms/tick | 変化 |
| --- | ---: | ---: | ---: |
| legacy | 28.840061 | 32.490153 | +12.66% |
| hyperpath | 15.429938 | 19.767888 | +28.11% |

各モード1回のサンプリング測定で、小さい差には測定ばらつきが含まれる。
前回からjgrpp-ya統合、キューの差し戻し、非同期構築なども入っているため、差のすべてを8ff4834dの単独変更へ帰属させない。
`gui.passenger_precomputed_workers` は今回の既定4。構築を強制せず、停止中のバックグラウンド準備完了を待ってから測った。

指定の `passenger_wpr_control.py` による補正も参考値として保存した。
処理件数と終了状態が変わる比較なので、同じ仕事量・共通の速度変動を仮定する補正を主な結論には使わない。
係数（新/旧）はLegacy 1.069825441、Hyperpath 1.356824996。

参考補正後の `RunPassengerWaitingEvents` inclusiveはLegacy 15.167122、Hyperpath 2.693885 CPU ms/tick。

測定条件と記録の検証:

- StateTicks 13,647,872 → 13,649,920。各2,048個の実tickサンプル。`--resume --warm-precomputed --wait-background-ready`。
- Hyperpath索引ON (`--hyperpath-filter-mode index`)。メインスレッドaffinity mask=1。再構築・cold条件・追加のトレースは使っていない。
- MSVC 19.44 x64 Release、`/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert ON、LTO OFF、同じCPU WPRプロファイルとWPAエクスポーター。
- 全763 nativeテスト、10,586,640 assertionsが通った。製品ソースへの追加変更はない。
- 両区間のkernelプロセス監査に競合ビルド・テスト・ゲームなし。lost events / buffersは0、製品関数の未解決シンボルなし。
- 生CSVのSHA-256、サンプル数とexclusive時間の保存則、支払い整合性を検証。WPA警告の時刻は有無を含め `validation.json` に保存し、すべて計測マーカー外であることを確認した。

入力の互換性:

前回の8462e6cd定常セーブは未公開の開発版形式（feature 3/3/17、JPSD 15）だった。
jgrpp-ya統合後の8ff4834dは整理後の形式1を受け付けるため、元入力のロードは拒否された。この試行はWPR開始前に失敗しており、結果に含めていない。
元セーブを保存したまま、コピーのfeature番号を整理し、製品側で固定された旧アルゴリズム設定など15 words（120 bytes）をJPSDから取り除いた。
シミュレーションを進めずに変換し、8ff4834dで0 tickロード・再保存検証を各モードで実施した。
初期canonicalの実状態22項目が元の定常入力と一致した。変換したJPSDと0 tickで再保存したJPSDは、全体がバイト単位で一致した。
形式に依存する `runtime_routing_feature` と `database_digest` の2項目は、この初期入力の確認から除いた。
現在のスイープ管理項目は旧形式ロード時の製品既定で初期化される。終了時canonicalの比較はしていない。

変換内容・旧新SHA-256・0 tick検証は `input-conversion.json` と `input-conversion-validation.json`、変換コードは `checks/convert_inputs.py`。
元入力と共有アセットのZIP、および変換済み入力ZIPを作業ツリー外に保持した。ETLと対応exe/PDBも作業ツリー外で保持し、ハッシュを確認した。
保存場所とSHA-256は `input-backup.json`、`converted-input-backup.json`、`recording-backup.json` に記録している。

[比較の全数値](measurements/passenger-wpr-generation-8ff4834d-2026-10-18/comparison.json)、
[関数別inclusive/exclusive差分CSV](measurements/passenger-wpr-generation-8ff4834d-2026-10-18/function-before-after.csv)、
[Legacy関数CSV](measurements/passenger-wpr-generation-8ff4834d-2026-10-18/legacy-functions.csv)、
[Hyperpath関数CSV](measurements/passenger-wpr-generation-8ff4834d-2026-10-18/hyperpath-functions.csv)、
[WPR・入力・生CSVの検証](measurements/passenger-wpr-generation-8ff4834d-2026-10-18/validation.json)。

機械補正の再計算:

```powershell
python -B utils/passenger_wpr_control.py docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/candidate docs/measurements/passenger-wpr-generation-8ff4834d-2026-10-18
```
