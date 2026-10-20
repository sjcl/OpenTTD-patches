# 88dc128a の原因別タイマーの補正（補遺）

[計測レポート](../../../passenger-wpr-abba-88dc128a-2026-10-20.md)は、Hyperpath の B 2 本で CPU 全体が遅く、`RoutePassengerJourneys` の時間差を判断していない。この補遺は同じ記録だけを使い、処理量が A/B で同じタイマーを物差しにして、原因別の再計画時間を比べる。レポート本体は変えていない（ハッシュは `report-sha256.json`）。

## 結論

**88dc128a は generation の再計画を減らしたが、その分がほぼそのまま `cause=new` に移った。再計画の合計はほとんど減らず、時間はむしろ 5〜8% 増えた。**

- Hyperpath の generation は 23,417 → 5,154 packet（−18,263）。同じ 2,048 tick で new は 20,578 → 36,259 packet（+15,681）に増えた。new と generation の合計は 43,995 → 41,413 packet（−5.9%）にとどまる。
- 補正後の new + generation の時間は A 408.0／411.2 ms、B 430.2／440.8 ms（2,048 tick あたり）。差は +0.011〜0.016 ms/tick。
- Legacy は両版で全原因の packet 数が一致し、canonical も A/B で一致した。Legacy の表がある間は全件 sweep のままなので、これは設計どおり。

## 補正の方法

expiry と failure の packet 数は、A/B 4 本すべてで同じ（Hyperpath: 202,283／185,185、Legacy: 267,790／90,310）。同じ処理なので、そのタイマーの比を、その本の CPU の速さの物差しにした。各本の全原因の時間を、01-A1 に対する（expiry + failure）の比で割る。

この補正は効いている。補正後の expiry は Hyperpath の 4 本で 273.2〜274.0 ms、failure は 621.2〜621.9 ms に揃う。補正前の B2 は 1.28 倍遅く、関数別 CSV でも `RunTileLoop` や列車の処理まで一様に +25〜30% 増えていた。このため、B の生の CPU 時間の増加はコードの差ではなく、計測機の状態と判断した。

| 本 | 物差し | new packet | new ms | generation packet | generation ms | new+gen packet | new+gen ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 01-A1/hyperpath | 1.000 | 20,578 | 167.9 | 23,417 | 240.1 | 43,995 | 408.0 |
| 02-B1/hyperpath | 1.067 | 36,259 | 370.9 | 5,154 | 59.2 | 41,413 | 430.2 |
| 03-B2/hyperpath | 1.280 | 36,259 | 375.7 | 5,154 | 65.1 | 41,413 | 440.8 |
| 04-A2/hyperpath | 1.013 | 20,578 | 184.6 | 23,417 | 226.6 | 43,995 | 411.2 |

ms は 2,048 tick あたりの補正後の原因別タイマー。全本・全原因の値は [cause-normalised.json](cause-normalised.json)、再現は `python -I cause_normalise.py <計測ディレクトリ>`（[cause_normalise.py](cause_normalise.py)）。

## なぜ移ったか（件数とコードからの推定）

- generation sweep が回るのは `routing_packets`（計画がまだ Journey でない packet）と、Hyperpath では `legacy_journey_packets` だ（`NextPassengerGenerationPacket`）。駅に着いたばかりの packet もここに入る。
- A では行の公開ごとに全件 sweep が走っていた。そのため、着いたばかりの packet の多くは、自分の routing event より先に sweep で計画されていた。原因は generation と数えられる。
- B では、新しい行の目的地でない packet は読み飛ばされる。その packet は自分の event で計画され、原因は new になる。
- つまり A の generation の大半は「捨てていた再計画」ではなく、どのみち一度は必要な初回の計画だった。省けたのは 2,582 packet だけ。
- B の new は 1 件あたりが重い（補正後 A 8.2〜9.0 µs、B 10.2〜10.4 µs）。sweep 経路には、変わらない fallback 待ちを `QueryWaiting` だけで済ませる近道がある。event 経路は常に `route()` を通る。増えた時間はこの差の可能性があるが、packet 単位の記録がないので確定していない。

## 判断

- 88dc128a は採用しない方がよい。保存フィールドを 3 つ増やし、挙動も変えたが、再計画の総量も時間も改善していない。jgrpp-ya にはまだ入っていない。
- generation の時間を減らすには、件数ではなく 1 件あたりを軽くするしかない。初回の計画は必要な仕事だからだ。
