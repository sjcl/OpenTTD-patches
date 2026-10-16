# 推定サービス対応の実測証跡

解釈・Phase別状態・残課題は `../../passenger-estimated-services-2026-10-16.md`。
製品runtimeのソースは `bea07371bd315d58f91c52a55243195cc91e5e01`。
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

`runs/quality-through-final` のCSVは標本の実乗降・未到着・消失と全体輸送を分離する。
初期標本は途中から観測する同一人物、新規完了標本は出発前から観測した旅行。
完了集合が異なる平均を同一人物の改善とは解釈しない。未到着と消失を平均に含めない。
`transport-target-events.csv` の候補コストはsample=0の参照照会であり、実Intentとは
別列。拒否理由は補助分類で、全列車の乗車機会を網羅せず、cache照会の重複もある。

`runs/quality-focused` と `quality-focused-summary.json` は独立した重点OD追跡。
通常の1/64抽出に代え、対象3 ODの各Packetの代表1人を追う。人数加重の全数調査ではなく、
通常標本の全体平均には混ぜない。出発時から観測した完了旅行と、旅行途中からの残り時間、
未到着・消失を区別する。`focused-canonical-comparison.json` と
`checks/focused-trace-integrity.json` で通常追跡とのゲーム状態・全旅客輸送の一致を照合する。
全項目・全旅客収支が一致し、未説明標本と目的地不一致は0だった。
`focused-od-details.json` はOD別に新規・初期・途中観測の到着/未到着/消失、時間分布と
照会理由を分離する。`checks/analyze-focused.py.gz` を展開し、
`python analyze-focused.py RUN_DIRECTORY OUTPUT_JSON` で生CSVから再集計できる。

初期cold試行で準備用statsがtraceを終了する計測器の問題を発見し、除外した。
`trace-selfcheck-summary.json` と `runs/trace-selfcheck` は修正後の自己検査。
小規模fixtureで初期snapshot後の目的地割当15人を捕捉した。
最終比較の全試行で標本収支の未説明数0、支払い検査正常。

## 性能

`cpu-final-summary.json` は3方式の4,096tick。旧2方式は今回初回の、変更のない
基準を再利用し、最終改良方式は乗り通し修正後に測り直した。
StateTicks: `3181095005..3181099101`。追跡なし、同時ビルド・テスト・別OpenTTD検出0。
simulation/Journeyはスレッド経過時間、process CPUはOS計測。読み込み・準備・保存を
含むOS CPU・壁時計・peak memoryと、通常tickの時間を区別する。
旧事前計算は保存snapshotからの準備、改良は表を破棄した後のcold準備。
単発試行であり、反復試験の信頼区間は求めていない。

## 保存・MP・製品テスト

`checks/save-readback.json` は長時間輸送終了・性能終了の2セーブを、tickを進めず
再読込したcanonical比較。derived rowsと操作数は初期読込比較から除外する。
その再生は合成テストとライブMPで別途検証する。

`multiplayer/mp-through-fixture` は全推定系統の3駅fixture。15人の実乗車・
配送、接続・設定OFF/ON・要求4,096tick後のPacket、DB、支払い、予約・所有権・
積載・表進捗を比較する。ネットワークpauseの到達待ちで実tickは超過する。
合成fixtureの目的地割当を実マップの品質試行には使用していない。

修正前の `../passenger-estimated-services-before-through-2026-10-16` には
起動準備中の参加失敗も保存した。進捗222件を受けても既定900秒で切断し、
成功データとして扱わない。最終版の `multiplayer/mp-through-real-ready` は
サーバー準備後の参加を検証する。起動準備中の最終版参加は再試行していない。
`--wait-server-ready` はハーネスの接続開始だけを遅らせ、nativeの期限・バリア・
分割再生を変更しない。各試行ともloopback、frame_freq=20、既定のゲーム期限。
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

独立した重点追跡は同じ品質バイナリ・入力・tick範囲で
`OPENTTD_PASSENGER_FOCUS_TRACE=1` も設定して別出力先で実行する。出力の
`process.json` と集計の `sampling_method` が抽出方法を識別し、3方式比較ツールは
重点抽出の混入を拒否する。通常の品質測定・CPU測定ではこの変数も削除する。

CPUはtrace環境変数を削除し、同ツールで`--ticks 4096`。
Legacyは前回CPUバイナリに`--no-precomputed`、旧事前計算は同じバイナリに
`--precomputed --warm-precomputed`、改良は今回CPUバイナリに
`--precomputed --cold-precomputed --warm-precomputed`。
`summarize`はprocess、raw tick、consoleから分布を作る。

MPは `passenger_precomputed_join.py BINARY PACKAGE INPUT OUTPUT LANG` に
`--frame-freq 20`、fixtureでは`--ticks 4096 --toggle-precomputed`、実マップでは
`--ticks 1024 --observe-timeout 1200 --wait-server-ready`。
fixtureは小規模セーブを`run --ticks 0 --fixture-estimated`で作り、実マップは品質終了セーブ。

修正前の全記録・報告は `../passenger-estimated-services-before-through-2026-10-16`。
そこからの実装差は、実車で安全かつ安い乗り通しとそのPacket分割回帰である。

`passenger_estimated_compare.py OUTPUT LEGACY PREVIOUS IMPROVED` が基準hashと時刻、
終了状態、標本収支を検査して比較する。`manifest.json`は各証跡の保存hashと
展開後hashを持つ。`.gz`はUTF-8テキストのgzipで、assetsや実行ファイルは含めない。
