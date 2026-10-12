# Trip予測キャッシュの保持上限拡大 — 2026-10-12

Trip予測の既定値・ハード上限を8→128 MiB、Capture全体を16→256 MiBへ、
それぞれ16倍に引き上げた。上限は共通定数にまとめ、ActiveContextと診断の
呼出側にも適用した。必要な分だけ確保し、上限到達時は通常予測へ戻る。
寿命・入力照合・期間一致・Work再現・論理Graph/Event予算は維持する。

旧8 MiBを超える結果を生成する256台の共有指令fixtureを追加した。
旧枠では一部を再計算し、新枠では256 Tripすべてがヒットする。
予測・停止状態・Work・論理メモリは一致した。ActiveContextの保存bytes/RNG/
tick allowance比較も新しい128 MiBで実行した。
Trip/Captureは **11 cases / 55,425 assertions**、CTestは **427/427、24.35秒**。

同じ実マップ・同じtickで、読み取り専用100 OD測定を3回実行した。
下表の旧枠の回数・保持量は前回測定、新枠の時間は今回の中央値。
時間を直接比較する無効側は、今回の同一バイナリで前処理を共有した条件である。

| 指標 | 旧8 MiB枠 | 新128 MiB枠 |
|---|---:|---:|
| 物理Trip予測実行 | 219,015 | 3,151 |
| キャッシュヒット | 61,424 | 277,288 |
| ヒット率 | 21.90% | 98.88% |
| Trip保持envelope | 8,387,594 B | 26,186,716 B（約25 MiB） |

今回のキャッシュ無効側は280,439回実行した。Graph構築時間は
無効2.715801 s→有効2.387035 s、中央値で12.1%減少した。
100構築、論理Work 70,407,374、予測Work 16,063,788、
Graph memory bound 79,085,888 B、最大Visit 178,068は一致し、
全3回で予測比較の不一致は0だった。
Capture保持量の推定は29,982,752 B。保持量は必要な結果に応じて増える。

これは停止したマップでのGraph測定で、全simulation tickの高速化を検証したものではない。
前回の32-tick実行ではTrip保持ピークが約5.3 MBで旧枠内に収まっていた。
今回確認した効果は、同じイベントで多くのODを扱い、旧枠が満杯になる場合の再利用増加である。

測定前後の保存SHA-256はともに
`91f1078975e6c09320837e8dbf734a0a6400c61a6a53198e0bc6f4bc58f29da1`。
入力SHA-256は前回と同じ
`14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`。
製品ソースに測定専用コードは追加していない。

[測定・検証メタデータ](measurements/passenger-trip-prediction-cache-memory-2026-10-12/metadata.json)、
[測定集計ログ](measurements/passenger-trip-prediction-cache-memory-2026-10-12/expanded-cache-measurement.log)、
[全CTestログ](measurements/passenger-trip-prediction-cache-memory-2026-10-12/ctest-expanded-cache.log)を保存した。
