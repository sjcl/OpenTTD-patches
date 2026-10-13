# 周期ダイヤの路線ベースTimed探索

ベース: `c2c4d1898c3a38a091ff224c5be660ede72238f7`。ブランチ: `feat/passenger-timed-round-based`。

`linkgraph.passenger_timed_rounds=true` と `passenger_journey_periodic=true` で、実際の `PassengerJourneyActiveContext::PlanWaiting()` が新探索を使用する。既定値は旧方式。方式はPATXに保存し、通常のゲーム設定コマンドで全クライアントへ同期する。新しい `passenger_timed_rounds` 拡張機能を持たないセーブでは旧方式を選択する。方式変更後も既存の有効な乗車Intentは実乗車検証を受け、次の再計画から選択した探索器を使用する。

## 探索と証明範囲

Round 0はOriginのみ。Round nはn回乗車したLabelを作る。前roundの更新駅から公開StopRefを列挙し、ServiceIDごとに重複を除く。同期的なsimulation threadの検索中には公開が進まないため、QueueのServiceポインタは同じincarnation/generationを指し続ける。

Serviceは最初の影響Occurrenceから最大2周期弱を走査する。最初の1周期で乗車し、後半は周期をまたぐ車内候補だけを運ぶ。個々の候補の寿命はN-1区間まで。駅IDが同じでもOrder Occurrenceを統合しない。NoLoad/NoUnloadと強制降車はCargoTypeごとのmaskを使う。車内候補を強制降車地点で消し、到着候補を作ってから次の乗車を評価する。通常車庫・Waypoint・運転停車は公開arrival/departure offsetの差に含まれる。

次発は既存DepartureIndex、NextDeparture、LoadingBaseの薄い公開ラッパー、認証済みDispatchスロットを使用する。車両の将来位置を展開せず、位相不明や未認証の操作から即時発車を作らない。探索後のServiceID・世代・Order・施設・容量・実乗降条件の検証は既存コードを使用する。

乗車中にDispatch Occurrenceを越える場合、その到着後の最小dwellから認証済みの次スロットを求め、車内の待機時間を後続到着と費用に加える。Dispatch地点で降りる候補の到着は待機前の時刻を保つ。車内支配条件にも乗車数を明示し、後発でも乗換ペナルティが少ない経路をround間で残す回帰を持つ。

乗換の次の乗車では直前Serviceを除外する。実際の `PlanWaiting()` と `CanBoard()` が同じincarnationへの再乗車を禁止するため、別Occurrenceや別共有車両への乗り直しも候補にしない。通し乗車は1つのService走査で残す。強制降車後に後続Occurrenceで同じServiceへ乗り直す経路を避け、実輸送へ接続する回帰で確認する。旧探索の参照経路にこの種類の接続があれば、実行条件が同じ参照として扱わない。

StationのPareto比較は到着時刻・費用・乗車数・先頭Intent順位に加えて、直前Serviceと降車Occurrenceを保持する。待ち時間に追加の重みがある場合、異なる到着時刻を早着だけで支配しない。車内候補も先頭順位と残り継続区間を比較する。完成した目的地Labelは挿入時に保持し、Work終了後も親を辿って全経路と先頭Intentを返す。

大きい予算では既存の逆向き緩和下界を再利用する。逆探索には全Workの1/4までを割り当て、実行可能な便時刻とは区別する。キャッシュキーは旧Timed=0、Schedule=1、round下界=2。下界のWork・冷状態のMemoryを再生するため、cache有無と途中参加で探索結果を変えない。

広いroundは目的地下界・費用・時刻・先頭Intent順位による128 Labelの幅制限を使う。これは近似であり、省略が発生すると最良性を証明しない。固定の最初の数roundで終了せず、疎な経路は最大乗換数または資源制限まで進む。影響Serviceは目的地に降車できるServiceを優先し、その後に下界とServiceIDで走査する。Queueを全国のServiceから作らない。

`PassengerTimedRoundResult` は既存 `PassengerPeriodicPlan` を保持する独立した結果アダプタで、Agent Aの状態拡張と統合する場所を限定する。

| outcome | 意味 |
|---|---|
| Found | 公開情報の範囲で探索を省略せず最良候補を確定 |
| Provisional | 完成経路あり。資源・幅制限・不明位相・公開途中のため最良性未証明 |
| BudgetDeferred | 資源または最大乗換数で停止し、完成経路なし |
| Unreachable | 現在の公開Timed情報と乗降条件では到達不能 |
| Indeterminate | 位相・公開・認証情報が不足し、完成Timed経路なし |
| Invalid | 不正なODまたは評価入力 |

`plan.deferred` は資源省略を示す。Provisionalには `ride` と完全な `route` があり、PlanWaitingはこれを採用する。位相不明の場合は既存Schedule-awareへ進み、それもなければUntimedへ進む。全経路はCargoPacketへ保存しない。到達不能の証明は今後の指令変更や物理ネットワークの永久的な断絶を意味しない。

## Workとメモリ

本番の予算は65,536 Work、8,192 Labels、8MiB。旧方式とはWorkの単位が異なる。

- Round開始、駅・Labelの取り出し、StopRef列挙、Occurrence、車内候補の前進・降車、発車評価を課金する。
- Fleet索引の冷状態構築をscanとsort相当で課金し、索引有効・無効の両方で同じFleet/ライブ停車/除外sort Workを再生する。
- Station支配比較は両方向の比較と削除フラグ更新をまとめて課金し、比較関数の呼出し数を別計測する。
- Frontier/Serviceのsort、filter、入力に比例するコピーを課金する。
- Label格納の追加1単位で後の親経路復元を前払いし、完成候補を次のWork停止で失わない。

基本4KiB、Labelごと1KiB、Queueノード128 bytesを保守的に見積もる。Labelの見積もりにはdequeのblock/map、3つの疎なStation map、ID vectorの余剰、返す経路を含む。車内候補・rank・発車除外などのvector capacityは追加課金する。逆探索の冷状態scratchと下界storageも計上する。Label/Memory制限による省略を明示する。Optional DBキャッシュの物理常駐容量と検索の論理メモリは別である。

追加vectorも固定envelope（車内候補256、Service行64、Label ID 8、車両/Service ID 4 bytes）と明示的な論理capacityで課金する。ポインタ幅・STLのreserve実装・物理的な再利用でMemory停止や経路が変わらない。

OS時計は診断のみ。検索の順序・許可・打切りには使用しない。

比較用ビルドには旧方式の前向きLabel試行、frontier比較、Service候補列挙と時刻評価に入るService展開も追加する。通常ビルドでは追加カウンターを更新しない。旧方式の比較数はfrontier要素の訪問、新方式は両方向のStation支配predicateと車内支配predicateの呼出しであり、同じ単価のWorkとは扱わない。

Windowsの比較用ビルドだけで `GetThreadTimes` のkernel+user CPU差をODごとに集計する。`Periodic query CPU` のsamplesが0なら未取得であり、CPU消費0とは扱わない。OSのCPU時計の粒度があるため、単一ODの値を順位付けせず多数ODの合計と平均を使う。`query_us` は経過時間、process CPUはロード・保存・workerを含む別の値である。通常ビルドではこの追加CPU時計を読まない。

## 検証・比較手順

`openttd_test "[passenger]~[.benchmark]"` と `passenger_timed_rounds_production` を実行する。新規回帰は普通/特急、位相と頻度、直通と乗換、0/1/2/4/11乗換、異周期、共有Fleet、認証/競合Dispatch、循環・繰返しOccurrence、Cargo別禁止、強制降車、運用停車、Work/Label/Memory暫定採用、変更・保存・途中参加を含む。既存4モードの実乗車・3回乗換・最終配送回帰も両方式で実行する。

`passenger_timed_compare 256` は同じ公開DBを3つの独立DBへ復元し、同じStateTicksで旧方式・新方式・大予算旧方式を比較する。通常65,536/8,192/8MiB、参照1,048,576/65,536/128MiB。待機Packetのreadyと直前Service除外も合わせ、最初の256個の異なる待機要求を使うため駅IDへの偏りがある。参照が延期、連続するLegのServiceが同じ、またはLegの途中でDispatch地点を越えるなら、認証済みの実行可能参照とは呼ばない。旧方式の車内offsetはDispatch地点での次スロット待機を含まないため、この区間の参照は別途時刻を確認する必要がある。結果・各Leg・処理件数をconsoleへ出す。実行は生産DB・Packet・RNG・simulationを変更しない。

`utils/passenger_timed_rounds_benchmark.py` は入力Save/資産を検証し、旧→新→新→旧を独立プロセスで同じ8,192 ticksに対して実行する。共有測定ロック、90秒のidle grace、5秒間隔の競合検出を使い、競合した測定は採用しない。言語ファイルは同じビルドのものを全条件で使用する。Workだけで優劣を判定せず、CPU、simulation分位点、実輸送、Fallback、メモリ、費用を報告する。

## 初期診断

128 Label幅・目的地下界を導入する前の固定256 ODでは新方式190経路、旧方式241経路。導入後は199経路。Work中央値は53,487.5から23,376.5へ減ったが、発見率は旧方式に届かなかった。参照経路と比較できた199経路では194同費用、5悪化、最大11,811 ticksの差。これは調整途中の証拠であり、最終実装の性能値には使用しない。目的地Serviceの優先走査と支配比較の整理を加えた最終版で再測定する。
