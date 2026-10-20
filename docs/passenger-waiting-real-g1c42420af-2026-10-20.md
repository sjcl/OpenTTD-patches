# 実マップのロード・途中参加後の待機時間（2026-10-20）

`g1c42420af.sav`で、基準 `8f200282e83246898de702d014aa6acda0a95905` と今回の `fe663cfa81661fc02cddb30e5be1618dcea0292f` を比較した。通常tickはユーザーの指定により対象外とし、採用した全待機計測で **StateTicks=3,201,034,745のまま** 準備を完了した。製品コードの追加変更はない。

既存の旧世代復元が直列だったため、GUIの27ms周期による待ちも重なっていた。今回の4 worker設定では、Legacyの復元待ちが約225秒から26秒、Hyperpathが169秒から14秒へ短縮した。実TCP参加の起動から初回同期完了までは185秒から24秒へ短縮した。

以下の「旅客準備待ち」は、最初の`PreparePrecomputed` / `StepLoad`から完了までの実時間で、GUI周期間の待ちを含む。「起動→完了」はプロセス作成からの時刻で、アセット初期化、ファイル読み込み、参加時の転送も含む。起動済みアプリのボタンクリックだけの時間ではない。参加完了は製品の初回syncが成功し、参加ダイアログを閉じる箇所で記録した。

| 条件 | 旅客準備待ち 基準→今回 秒 | 変化 | 起動→完了 基準→今回 秒 |
| --- | ---: | ---: | ---: |
| 初期構築 Legacy | 38.191 → 21.754 | -43.04% | 42.837 → 27.029 |
| 初期構築 Hyperpath | 26.003 → 12.427 | -52.21% | 30.853 → 16.911 |
| 旧世代を含む復元 Legacy | 225.379 → 26.326 | -88.32% | 230.105 → 30.742 |
| 旧世代を含む復元 Hyperpath | 169.277 → 13.790 | -91.85% | 173.640 → 18.132 |
| TCP参加 Hyperpath | 176.507 → 14.633 | -91.71% | 184.863 → 23.552 |

要求worker数は両版とも4。初期構築の実worker数はLegacyが3（既存メモリ予約上限による制限）、Hyperpathが4だった。以下のコア相当は、`pax-destination`という名前のworkerだけのOS CPU時間合計を準備実時間で割った値。全32論理CPUに対する使用率ではない。プロセスCPUには描画やロードしたCargoDist背景計算も含むため、worker CPUと混同しない。

| 条件 | worker CPU 基準→今回 秒 | worker平均稼働 基準→今回 コア相当 | main CPU 基準→今回 秒 |
| --- | ---: | ---: | ---: |
| 初期構築 Legacy | 49.297 → 50.594 | 1.29 → 2.33 | 12.656 → 14.844 |
| 初期構築 Hyperpath | 64.375 → 48.234 | 2.48 → 3.88 | 9.078 → 1.766 |
| 旧世代を含む復元 Legacy | 0.000 → 58.375 | 0.00 → 2.22 | 75.266 → 18.953 |
| 旧世代を含む復元 Hyperpath | 0.000 → 54.172 | 0.00 → 3.93 | 55.312 → 2.531 |
| TCP参加 Hyperpath | 0.000 → 57.125 | 0.00 → 3.90 | 58.906 → 2.000 |

旧世代復元では基準版のworker CPUは0だった。今回Hyperpathは約3.9コア相当で、4 workerをほぼ継続稼働させている。初期構築でも今回の全4,936行がworker hit、serial_rows=0となった。Legacyの残る待ちは、復元のグラフ準備約6.7秒と、旧世代行の復元約19.6秒に分かれる。復元時の旧世代worker数そのものはこのreadiness記録に含めていない。

| Hyperpathの今回版 | worker 4 準備待ち 秒 | worker 8 準備待ち 秒 | worker 8 起動→完了 秒 | worker 8 平均稼働 |
| --- | ---: | ---: | ---: | ---: |
| 初期構築 | 12.427 | 7.015 | 11.757 | 7.75コア相当 |
| 旧世代復元 | 13.790 | 7.060 | 11.511 | 7.72コア相当 |

peak working setは起動・読み込みも含むプロセス全体の最大値で、準備窓だけの増分ではない。

| 条件 | 基準 peak GiB | 今回 peak GiB |
| --- | ---: | ---: |
| 初期構築 Legacy | 4.387 | 4.424 |
| 初期構築 Hyperpath | 6.727 | 6.698 |
| 旧世代を含む復元 Legacy | 4.388 | 4.475 |
| 旧世代を含む復元 Hyperpath | 6.657 | 6.725 |
| TCP参加 Hyperpath | 6.661 | 6.726 |
| 初期構築 Hyperpath worker 8 | — | 6.740 |
| 旧世代復元 Hyperpath worker 8 | — | 6.769 |

8 workerではHyperpathの復元が約7.1秒となり、worker CPUは約7.7コア相当（8本あたり約96.5%）だった。この補足は同じ版のローカル設定だけを変えた試験で、8 workerでのTCP参加時間は測っていない。ユーザーの通常設定ファイルは変更していない。

入力は48,312,608 bytes、SHA-256 `22743c39125f5ac35a813144506040f3693b40d0f374514a0bb8c3c20eed9bb7`。元ファイルをコピーしてcfg・全アセットを凍結した。元セーブは内部形式v1なので、両版とも航空機コスト用の移動／待ち分離情報を一度再取得する。この初回ロードの初期構築と、v2保存状態の旧世代復元を分けて測った。

復元用の共通入力は同じマップを0 tickのまま構築し、乗換ペナルティ20→17で世代を切り替えて保存した。旧世代4,936行、新世代0公開行・7 Workを保持する。両版の完了時load_workはLegacy **5,806,592,131**、Hyperpath **1,973,842,621**で一致し、保存済み新世代Workも7のまま。不要行skipやWork上限の変更による短縮ではない。

TCP参加は同一PCの実暗号化loopback接続、専用サーバー＋Windows GUI観戦クライアントで行った。基準版同士／今回版同士を接続し、ホストをポーズしたまま参加した。参加用コピーには保存済みのErrorポーズがあり、最初の専用サーバーは製品の判定で拒否した。既存の計測用`unpause`で解除して0 tick再保存した入力を両版で使用し、解除前後のcanonical全24項目一致を確認した。元セーブと元の復元用コピーは保持し、専用サーバーの拒否判定やNewGRFを改変していない。NewGRFの容量変更警告はログに残っている。

Windows / ClangCL 23.1.3、x64 Release `/O2 /Ob2 /MT /Zi`、assert ON、LTO OFF、PGO OFF。実Win32 GDIドライバの既定の別ゲームスレッドを使用し、640×480・非表示で同じGUI周期を実行した。描画負荷や初期化量は普段の画面サイズと異なり、CPU・回線が異なる別PCへの性能保証ではない。各本は新規プロセス、OSファイルキャッシュは消去していない。主比較は各1本なので、小差の有意性は主張しない。

初期Legacyの最初のnative値は35.704秒、外部監視が揃った追加試行は38.191秒だった。最初は終了を先に検出したPython監視側の競合で最終census/peak metadataが残らず、生のready/canonicalは保持した。以降は終了後もファイルを読み、監視記録を随時保存するよう修正した。表のLegacy基準は追加試行を使用する。今回Hyperpathのpilotは12.800秒、主比較は12.427秒だった。失敗や別条件を混ぜて平均していない。

最長の準備API呼出しはLegacy初期で基準37.9ms／今回85.0ms、Legacy復元で98.6ms／51.7ms。Hyperpath復元は9.4ms／13.1ms、8 workerは26.8msだった。これはOSの中断も含む壁時計であり、GUI全体のフレーム時間ではない。外側8msの目標は単一API呼出しの厳密な上限ではなく、大きな割当・グラフ準備などの応答遅延は残る。

採用した全ケースで時刻不変、版間・worker数間のcanonical **24項目一致**、実参加ではホスト／クライアント間も24項目一致。最終計測バイナリのnativeは基準801ケース／10,757,738 assertions、今回805ケース／10,523,428 assertionsが成功した。通常tickのfilter validateは対象外・未実施で、canonical一致をvalidate成功と呼んでいない。

当初の通常tick採取はユーザーの対象変更で追加を停止し、完了済み2本も今回の性能結論から除外した。WPR pilotは検証済みkernel専用profileで欠落0・両マーカー・順序を確認したが、ここで示す待機時間とCPUはGUI／初回syncのマーカーとOS CPU時計による値であり、2,048 tickのCPUサンプル値ではない。

入力ZIP・派生セーブ・両版のexe/PDB/lang・使ったfixture生成exe/PDB・pilotと除外ETLは、作業ツリー外の `C:\Users\bebe\OpenTTD-measurements\passenger-waiting-real-20261020-fe663cfa` に独立コピーで保管し、SHA-256を記録した。元セーブのhash不変とZIP内の全アセットも照合した。

- [全生値・CPU・フェーズ](measurements/passenger-preparation-real-g1c42420af-2026-10-20/summary.json)
- [採用・native結果](measurements/passenger-preparation-real-g1c42420af-2026-10-20/validation.json)、[canonical照合](measurements/passenger-preparation-real-g1c42420af-2026-10-20/canonical-comparisons.json)
- [入力manifest](measurements/passenger-preparation-real-g1c42420af-2026-10-20/input/package.json)、[独立保管manifest](measurements/passenger-preparation-real-g1c42420af-2026-10-20/backup-manifest.json)
- [再現用スクリプト](measurements/passenger-preparation-real-g1c42420af-2026-10-20/checks/)、[不採用・失敗の扱い](measurements/passenger-preparation-real-g1c42420af-2026-10-20/attempts.json)
