# MP起動準備中参加の証跡

解釈・期限モデル・制約は [報告](../../passenger-server-preparation-deadlines-2026-10-16.md)。
基点 `674d5ab12e`、実装 `5583fc2feb`、計測ツール `b661c7822c`。
ネットワーク期限の既定値は変更せず、サーバー自身の準備待ちとクライアント参加時間を
別の累積900秒で制限する。進捗・再生・待ち行列の再移行でリセットしない。

## 内容

`summary.json` は3試行の結果、期限回帰と製品CTest、製品ソースと計測ソースの一致を記録する。
各 `multiplayer` ディレクトリには結果、初期/終了snapshot、再生測定、設定、consoleと
圧縮stdout/stderrを保存する。バイナリ・セーブ・assetそのものは含めない。
`provenance` は使用したコピーTUとruntimeソース・バイナリのSHA-256。
最終製品に計測TUが残っていないことも検査する。

実マップは前の品質試行終了セーブ。SHA-256は
`306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`。
前回の準備済みサーバー試験と同じ入力で、今回はサーバー準備が終わる前に接続する。
停止試験は旧legacy入力 `cc73ec5d569d9582f96e42aae67d77022ac085e64683db36394930f3a24e6b68` を使い、
クライアントの最初の実再生進捗を確認してから、その試験のクライアントプロセスだけを一時停止する。
偽の進捗は送らず、既定500tickの無活動監視による切断を確認して回収する。
小規模fixtureは前の全推定3駅セーブを再利用し、設定OFF/ONのバリアも確認する。

経路DB、Packet、支払い、予約・所有権・積載、表進捗、経済カウンタを同一StateTicksで照合する。
FNV状態digestは回帰検査用で、暗号学的証明ではない。SHA-256は証跡の来歴検査に使う。
既存JP+ Multiple Unitsの容量変更警告と、計測用consoleによるerror pause解除もログに保存する。
loopback・同一バイナリ・frame_freq=20での単発試行であり、全ネットワーク環境を保証しない。

## 再現

MSVC x64 Release、assert ON、LTO OFF。既存のasset package、上記入力と対応するlangを用意する。
CMake生成後に `python utils/passenger_estimated_prepare.py SOURCE BUILD REVISION --mp` を実行して
計測用コピーTUをビルドする。製品ネットワーク・期限・再生は変更しない。

`python utils/passenger_precomputed_join.py BINARY PACKAGE INPUT OUTPUT LANG` に、次を渡す。

- fixture：`--frame-freq 20 --ticks 4096 --toggle-precomputed --observe-timeout 180`
- 起動準備中の実マップ：`--frame-freq 20 --ticks 1024 --observe-timeout 1900`
- 実再生停止：`--frame-freq 20 --initial-build --stall --ticks 0 --observe-timeout 1900`

起動中試験に `--wait-server-ready` は付けない。1900秒はハーネスの観察窓で、ゲーム設定を変更しない。
`server_owned_preparation_seconds` と `client_join_budget_seconds` はMAP_OK時点のサーバー計測値。
`join_elapsed_seconds` は接続開始からハーネスが参加結果を観測するまでの経過時間で、範囲が異なる。
クライアント予算はMAP_OK後の追い付きにも引き継ぎ、Activeになるまで失効検査を継続する。

CMakeを通常再生成して製品TUへ戻し、再ビルド後に全CTestを実行する。
`manifest.json` は保存バイトとgzip展開後の両方のSHA-256を記録する。
このディレクトリの自動改行変換を `.gitattributes` で無効にし、Windowsのcheckoutでも測定バイトを保つ。
