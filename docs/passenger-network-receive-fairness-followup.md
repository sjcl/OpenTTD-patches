# Multiplayer receive fairness under expensive catch-up

Work Admission integrationとは独立したネットワーク堅牢化課題として扱う。通常のcache有効プレイと途中参加をWork Admissionの統合条件とし、全derived cache / Scratch無効時の接続維持はこの課題のストレス条件とする。通常条件での決定性・Save/Loadの不一致を許容するものではない。

## 確認済みの機構

[原因分離レポート](passenger-query-work-admission-network-cause-2026-10-14.md)とその原本を参照。無競合の実マップ2再現で、clientは306〜352frameを一括で処理し、23〜24秒間NetworkReceiveへ戻らなかった。その96%以上がgame thread CPU時間。同期間もserverは送信し、切断直前のclient socketには24〜27KBが待機していた。catch-up直後、到着済みデータを読む前の20秒監視で切断した。

network.cpp / network_client.cpp / network_server.cpp / tcp.cppの製品コードは比較ベース8a96044d40から変更していない。これはdesync検出による切断ではない。Work Admissionがこの機構の発生頻度へどの程度影響するか、各cacheとScratchの個別CPU費用は未分離。

## 別修正で検討する内容

- 固定frame数でcatch-upを区切り、受信とsocket進行へ定期的に戻る。
- catch-up後の接続監視で、到着済みデータを処理する順序を検討する。
- receive fairnessだけでは持続的なCPU throughput不足は解消しないため、clientの重い処理を別に計測・軽減する。

探索をCPU時間・wall時間で打ち切らない。タイムアウトを単純に延長して合格扱いにしない。simulation frame / command順序、logical Work、乗車・配送判断、保存状態を保つことを検証する。

## 受入れ条件

実マップloopbackでserver・初期client・途中client、sync_frequency=1、通常CPU affinityを使用する。通常cache有効条件を維持し、全cache/Scratch無効のstress条件でも十分なframe数を進める。受信入口の復帰間隔、serverのOS送信成功、clientのpending bytes、thread/process CPU、frame backlogを記録する。CPU負荷が接続維持後も発散しないことを確認する。

同tickのCargoPacket全保存対象フィールド・支払情報と同期チェックを照合し、assert・切断・異常終了を検出する。無競合の反復だけをCPU比較に用いる。キャッシュ無効のstress failureを通常条件の成功へ置き換えず、この課題の結果として保持する。
