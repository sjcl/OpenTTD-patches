# 乗り通し修正前の比較記録

runtime c294b447d3での段階的な実測。最終版の結果とは分けて扱う。
当時の報告は [report.md](report.md)。

# 推定サービス対応の実測証跡

解釈・Phase別状態・残課題は `../../passenger-estimated-services-2026-10-16.md`。
製品runtimeのソースは `c294b447d3086788a49aa67b9c76ccd2c928f3b9`。
後続の計測ツール・報告コミットは製品の `src` を変更していない。
`runtime-provenance.json` に変更済みruntimeファイルのSHA-256、製品・測定バイナリの
SHA-256、最終製品ソースとの一致を記録する。バイナリとセーブ自体は含めない。

## 品質比較

入力SHA-256: `6783a500f47435a342a36e9a6544e33bb38f041abe91e9e2ff2c654edd16c978`。
StateTicks: `3181095005..3181119581`、24,576tick。

`quality-final-comparison.json` はLegacy・旧事前計算・改良の3方式比較。
旧2方式の生データは前回の `passenger-routing-integration-2026-10-15` のmanifestと
照合した。runtime a2915e1844と指定基点6f07b76078の`src`差分は空。
旧事前計算の記録には単発のコンパイラ検出があり、その時間はCPU評価に流用しない。
改良方式は同時ビルド・テスト・別OpenTTDの検出0、cold捕捉と全表構築後に開始。

`runs/quality-validated` のCSVは標本の実乗降・未到着・消失と全体輸送を分離する。
初期標本は途中から観測する同一人物、新規完了標本は出発前から観測した旅行。
完了集合が異なる平均を同一人物の改善とは解釈しない。未到着と消失を平均に含めない。
`transport-target-events.csv` の候補コストはsample=0の参照照会であり、実Intentとは
別列。拒否理由は補助分類で、全列車の乗車機会を網羅せず、cache照会の重複もある。

初期cold試行で準備用statsがtraceを終了する計測器の問題を発見し、除外した。
`trace-selfcheck-summary.json` と `runs/trace-selfcheck` は修正後の自己検査。
小規模fixtureで初期snapshot後の目的地割当15人を捕捉した。
最終比較の全試行で標本収支の未説明数0、支払い検査正常。

## 性能

`cpu-final-summary.json` は今回順次測り直した3方式の4,096tick。
StateTicks: `3181095005..3181099101`。追跡なし、同時ビルド・テスト・別OpenTTD検出0。
simulation/Journeyはスレッド経過時間、process CPUはOS計測。読み込み・準備・保存を
含むOS CPU・壁時計・peak memoryと、通常tickの時間を区別する。
旧事前計算は保存snapshotからの準備、改良は表を破棄した後のcold準備。
単発試行であり、反復試験の信頼区間は求めていない。

## 保存・MP・製品テスト

`checks/save-readback.json` は長時間輸送終了・性能終了の2セーブを、tickを進めず
再読込したcanonical比較。derived rowsと操作数は初期読込比較から除外する。
その再生は合成テストとライブMPで別途検証する。

`multiplayer/mp-final-estimated-fixture` は全推定系統の3駅fixture。15人が実乗車・
配送され、接続・設定OFF/ON・4,179tick後のPacket、DB、支払い、予約・所有権・
積載・表進捗が一致。要求4,096tickに対しネットワークpauseの到達待ちで超過した。
合成fixtureの目的地割当を実マップの品質試行には使用していない。

`multiplayer/mp-final-real-map` は起動準備中に接続した失敗試行。進捗222件を
受けても既定900秒の絶対参加期限で切断。成功データとして扱わない。
`multiplayer/mp-final-real-ready` はサーバー準備後に接続する別条件の試行。
`--wait-server-ready` はハーネスの接続開始だけを遅らせ、nativeの期限・バリア・
分割再生を変更しない。両試行ともloopback、frame_freq=20、既定のゲーム期限。
観測timeoutとnativeの絶対参加期限は異なる。

実マップ入力のJP+ Multiple Unitsには既存の容量変更警告がある。測定専用consoleで
NewGRF error pauseを解除して進行させ、警告・解除を生ログとprocess.jsonに保存した。
製品の警告・pause処理は変更していない。

`checks/ctest-product-final.log.gz` はコピーTUを外した製品ビルドの全CTest。
`checks/synthetic` は目的地表照会、物理車両数、OD多様性、共通区間、乗車索引、
Packetイベントの独立したCPUベンチマーク。実マップの時間とは区別する。

## 再現手順

MSVC x64 Release、assert ON、LTO OFF。詳細は `build-flags.txt`。
`utils/passenger_estimated_prepare.py SOURCE BUILD REVISION` をCMake生成後に実行し、
コピーTUをビルドする。品質は既定mode、CPUは`--cpu`、MPは`--mp`を使う。
通常のCMake再生成で製品TUに戻る。`provenance` に使用したコピーTUも格納する。

品質は `OPENTTD_PASSENGER_TRACE=1` を設定し、`passenger_precomputed_real_map.py run`
へ `--resume --precomputed --cold-precomputed --warm-precomputed --quality-probe --ticks 24576`
を渡す。`--input`で上記SHA-256のセーブ、対応する`--lang`と固定asset packageを指定する。
packageの既定inputとは異なるため、`--input`の明示が必要。

CPUはtrace環境変数を削除し、同ツールで`--ticks 4096`。
Legacyは前回CPUバイナリに`--no-precomputed`、旧事前計算は同じバイナリに
`--precomputed --warm-precomputed`、改良は今回CPUバイナリに
`--precomputed --cold-precomputed --warm-precomputed`。
`summarize`はprocess、raw tick、consoleから分布を作る。

MPは `passenger_precomputed_join.py BINARY PACKAGE INPUT OUTPUT LANG` に
`--frame-freq 20`、fixtureでは`--ticks 4096 --toggle-precomputed`、実マップでは
`--ticks 1024 --observe-timeout 1200 --wait-server-ready`。
fixtureは小規模セーブを`run --ticks 0 --fixture-estimated`で作り、実マップは品質終了セーブ。

`passenger_estimated_compare.py OUTPUT LEGACY PREVIOUS IMPROVED` が基準hashと時刻、
終了状態、標本収支を検査して比較する。`manifest.json`は各証跡の保存hashと
展開後hashを持つ。`.gz`はUTF-8テキストのgzipで、assetsや実行ファイルは含めない。
