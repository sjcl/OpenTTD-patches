# JourneyPlanner実マップの予測制限・再測定（2026-10-12）

対象ブランチは `fix/passenger-journey-real-map-scaling`、ベースは
`jgrpp-ya` の `67d5b90bb9`。元の実装コミットは
`cf2ae8e992ec296f932c994646d20a804fd10491`。
[前回測定](passenger-journey-real-map-2026-10-12.md)と
[構造インデックスの設計](passenger-service-graph-scaling.md)を踏まえた修正である。

過大なVisit予約、短周期便の128 Visit打ち切り、同等な完了経路による
検索ラベル消費を修正した。128 MiBの標準グラフ上限は維持した。
完全性を証明できない暗黙停車が見つかったScoped Graphは残りの予測を止める。
修正は全国規模の合成ネットワークで検索・実積載まで検証した。
実マップではメモリ失敗が解消した後も暗黙停車や時刻の不確定が残る。
検索数を増やすためにこれらをCompleteに変更していない。

## 原因と修正

### メモリ予約

旧実装はRunning Tripごとに `1024 + 320 * visit_limit` を予測前に計上した。
128 Visitなら41,984 bytesで、早い段階で予測不能になり数Visitしか生成しない便も
同じ量を払った。4,000便ならVisit予約だけで167,936,000 bytesとなり128 MiBを超える。
前回実マップの保持グラフ推定量は約20 MiBだったが論理予約は128 MiBに達した。

`passenger_service_graph.cpp` はTripの基本領域1,024 bytesを収録前に確保し、
予測器が実際にVisitを追加する直前に320 bytesずつ決定的に確保する。
予測器は論理Visit数から明示的に幾何拡張する。Visitの旧・新バッファの重複、
駅Visit・発車インデックス、Trip索引、Order・Dispatchコピーと予測作業領域を
従来の保守的係数で覆う。Visit/Tripについては`sizeof`の静的検査も追加した。
領域を確保できなければ割り当て前にLimitReachedを返す。
実測RSSや`vector::capacity()`は診断だけで、予算判断には使わない。

構造インデックスの冷・温キャッシュは同じ論理Work・ピーク上限を払う。
動的失敗したグラフの長期キャッシュは追加していない。

### 予測期間と高頻度便

Active実行は4,096 tickから始め、必要なら16,384、65,536 tickへ延長する。
各構築・検索は従来の停車イベントとtick予算を消費する。
ActiveのVisit上限を1 Trip当たり4,096へ変更した。
全体Visit上限262,144、Step上限4,096、Work上限2 Mi単位、グラフ128 MiBは維持した。
汎用Graph APIの既定Visit上限128は維持し、呼び出し元が明示的に延長を選ぶ。

短い期間内で得た経路を採用するには、CompleteなOD候補に加え、
非負の一般化費用が期間長より**厳密に小さい**ことを求める。
期間の終端以降のイベントを使う経路は、経過時間だけで期間長以上になるため、
その解を改善できない。費用が期間以上、またはPredictionHorizonなら期間を延長する。
最大期間でも証明できなければFallbackする。
時刻不明・分岐・Refit・競合Dispatchを単なる期間不足として延長し続けない。
予測の`complete_until`と排他的終端の契約、Scoped Completeを維持する。

### 同等便と候補

`passenger_journey_planner.cpp` は目的駅に到着済みの候補を要求されたK件に絞り、
元のLabelOrderによる時刻・乗り換え・容量確度・物理便・Visitの順序を保つ。
目的駅への代替候補を保存できると確認してから既存候補を置き換える。
中間駅のラベル安全上限は変更しない。

既にK件の完了解がある場合、非負の経過時間・待機・乗り換え費用の下界が
完了解の費用を**厳密に超える**発車・後続Visitを走査しない。
同費用の便は容量・物理便のtie-breakのため残す。
構造上関連する普通・特急・環状線・幹線迂回のServiceを恣意的に省略していない。
構造インデックスには最大ブロック内Service数の診断を追加した。

### 失敗後の無駄な処理

資源制限で失敗したGraphの発車索引を構築しない。
Scoped Graphでは、収録したTripが期間を覆わず暗黙停車の安全性を証明できない場合、
または無限定の暗黙運行がある場合、Opaqueを確定して残りの動的予測と発車索引を止める。
既に完全な期間を覆う便の後方にある暗黙停車は、この停止理由にしない。
残りの候補数は選択段階で記録し、未収録数の診断は下限として表示する。

この早期停止は一回のBuildに限る。次イベントのBuildは指令・車両進捗・容量を
読み直す。1,001便の回帰テストでは不確定な先頭1便だけを予測し、
暗黙指令を時刻付き停車へ直した次のBuildでは全便を収録してCompleteへ回復する。
停車イベント内での同一OD共有、tick予算、公平な待機共有枠は維持する。

## 診断

`passenger_routing.{h,cpp}` は予測の実用ステータスとは独立した停止理由と
予測カーソルを追加した。`passenger_service_graph.{h,cpp}` は最初のGraph失敗と
最初のTrip失敗を別々に記録する。

- Graph: Work、Memory、Services、Trips、TotalVisits、Index、Opaque。
- Trip: Work、Memory、Visits、Steps、Timing、Conditional、Refit、Implicit、
  Unsupported、Dispatch、Facilities、VehicleState、OrderProgress。
- Graph失敗: 最初の段階、消費Work、論理メモリ、収録済みTrip/Visit。
- Trip失敗: 車両ID、同じ消費量、予測カーソル、認証済み終端、要求終端。
- OD候補: eligible/selected Service数、選択Trip数、最大構造ブロック、
  実Visit予約数、最大未認証tick幅、CompleteなOD Graph数。
- 実輸送: Journeyの実乗車・乗り換え・最終配送人数。

`passenger_journey_active.{h,cpp}` がtick/累計診断へ集計し、
`console_cmds.cpp` の `passenger_journey_stats total|tick` が表示する。
最初のOpaque Service/Vehicle IDも出す。
Graph失敗Noneは資源/不透明スコープの直接失敗がないことを示す。
個別Tripの時刻が未知なら、これとOD GraphのComplete件数は一致しない。
Trip停止件数は予測を実行した分のみ。早期停止後の未収録Tripには停止理由を捏造しない。
Fallback/延期件数はCargoPacketの処理観測で、Graph失敗の構築件数とは単位が異なる。

`cargoaction.cpp` と `cargopacket.cpp` の計測は実際に移動した人数を数える。
予約は乗車として数えず、Gradual Loadingの予約→Keep変更時にだけ加算する。
混在するLegacy/Journeyの予約や部分積載も、その移動範囲だけを数える。
診断値・実時間はルーティング判断やセーブ形式に追加していない。

## 同一実マップの比較

元セーブは変更していない。前回の最終`verified-result.sav`を各測定用にコピーした。
Passenger CargoDist automatic(2)、OD=true、Journey=true、recalc_interval=8、
recalc_time=32、win32表示、MSVC x64 Release /MT、assert有効で測定した。
前回のベースバイナリはSHA-256も前回metadataと一致する。
入力には4,726ノードの旅客GraphとOD確定116,097人があり、前回測定で
全既存の非単一Graphの更新を確認済み。今回の終端でも全15ジョブが完了済み。

今回の比較はstate tick 3,176,545,487から3,176,547,014までの1,527 tick。
117528-03-11から翌月開始まで進めて自動採取・保存・終了する。
前回の30分以上/14,810 tickの累計値と直接比率を取らない。
NewGRF欠落と既存GRFの容量変更警告は両側にあり、ユーザーの了承の上でコピーを測定した。
この環境を完全なNewGRF一致環境やライブマルチプレイ検証と見なさない。

| 同一1,527 tickの観測 | 修正前 | 最終修正版 |
| --- | ---: | ---: |
| Graph構築 | 24,385 | 24,369 |
| 平均 / 最大構築時間 | 31.271 / 77.849 ms | 10.191 / 16.663 ms |
| 構築時間合計 | 762.557秒 | 248.361秒 |
| tick内Graph/Search計測時間の最大 | 796.711 ms | 210.369 ms |
| 論理Graph Work累計 | 11,825,036,662 | 13,697,260,849 |
| 最大保持Graph推定 | 20,124,758 bytes | 5,491,614 bytes |
| 最大論理Graph上限（構造索引込み） | 128 MiB | 29.15 MiB |
| プロセス全体peak Working Set | 2,087,284,736 bytes | 2,085,933,056 bytes |
| プロセス全体CPU時間（ロードを含む） | 973.813秒 | 439.719秒 |
| GraphMemory Fallback / 延期（packet観測） | 25,918 / 100,021 | 0 / 0 |
| Graph失敗 Work / Opaque（構築件数） | 原因別件数なし | 862 / 23,507 |
| 認証済みOD Graph | 未計測 | 0 / 24,369 |
| 検索 / 適用人数 | 0 / 0 | 0 / 0 |
| Legacy / Journey / Fallback在庫人数 | 394,787 / 0 / 75,381 | 385,775 / 0 / 84,522 |
| OD確定在庫人数 | 161,350 | 161,723 |
| 待機 / 延期在庫人数 | 203,038 / 57,759 | 203,731 / 48,452 |
| 資源上限によるTrip予測打ち切り | 8,394,104 | 5 |
| Journey実乗車 / 乗り換え / 最終配送人数 | 独立計測なし | 0 / 0 / 0 |

構築時間合計は約67.4%、最大tick内計測時間は約73.6%減った。
構築回数はほぼ同じであり、長時間の失敗キャッシュによる回数削減を主張しない。
メモリ上限を早期に使い切らなくなったため収録するTripが増え、
論理Workは約15.8%増えている。Workは決定的な安全予算でCPU時間と同じ単位ではない。
Visit数減少と認証不能なGraphの索引処理省略によって実時間を削減した。
プロセス全体ピークメモリはほぼ同じで、マップ・NewGRF・CargoDist workerを含む。
Graphだけの保持量減少を、ゲーム全体のRSS削減とは報告しない。

最終ランのTiming停止は11,018,262、Unsupportedは8,406,452、
VehicleStateは223,396、OrderProgressは95,629予測。
Visits/Steps/Memory停止は0。5件のTrip Work停止のほか、探索・構造処理などで
Graph Work制限に達した構築を合計862件観測した。
PredictionIncomplete Fallbackは34,161 packets、SnapshotLimit延期は362,323、
QueryWork延期は21,439、GraphWork延期は1,086 packetsだった。
従来のメモリ失敗を除去したことで、時刻・指令の不確定が表面化した。

最初のOpaqueスコープはService **714**、Vehicle **16563**。
宣言経路の暗黙停車を含む候補が要求期間を覆うことを認証できず、
Prediction段階、Work=585,459、論理メモリ=30,102,080 bytes、
収録2,679 Trip/30,311 Visitの時点で中止した。
最初の個別Trip失敗はVehicle **3355**のTimingで、
認証済み終端3,176,547,409に対して要求終端3,176,549,584（差2,175 tick）。
名目予測カーソルは3,176,558,425まで進んでも時刻が未知であり、
その値を完全性の証明に使えない。これらはVisit 128の打ち切りとは別の障害である。
列車の現在・将来の物理時刻を捏造してこのスコープを除外する修正は行っていない。

Console原本、機械可読の条件・結果・ハッシュ、プロセスメモリ、合成テストの証拠は
[測定フォルダ](measurements/passenger-journey-scaling-2026-10-12/metadata.json)に保存した。
セーブ・NewGRF・バイナリはリポジトリへ追加していない。

先行する修正版ラン（早期Opaque停止追加前）は同じ1,527 tickで、
Memory失敗0、Work失敗1,151、Opaque失敗23,232、OD Graph Complete 0、検索/適用0だった。
構築24,383回、平均26.556 ms、合計647.536秒。
時刻不確定13,034,796予測、未対応指令9,656,916予測、
VehicleState 326,101、OrderProgress 121,474を観測した。
最大構造ブロックは643 Service、選択Serviceはeligibleの約82.5%だった。
網状ブロックでは構造だけで多くの関連便を除外できない。
この実測を受けてOpaque確定後の早期停止を追加した。

## 回帰・ピークメモリ

最終MSVC x64 Release /MT、assert有効:

- `[passenger]`: 183ケース、135,341アサーション成功（明示タグで隠れた全国規模ケースも含む）。
- CTest: 400/400成功、53.22秒。旧/current save、GUI/診断のセーブハッシュ、
  無効時の従来挙動、Shadow、ゲームスクリプト互換のプロダクションランナーを含む。
- 4,000便・104,000 Visit: CompleteなOD Graphと検索が成功。
  Work=132,058、論理ピーク上限=37,896,128 bytes (36.14 MiB)、
  保持Graph推定=13,245,444 bytes (12.63 MiB)。
  独立実行のcold=17.066 ms、warm=16.160 ms（単回の診断、速度保証ではない）。
- 同テストプロセス全体: peak Working Set=45,613,056 bytes (43.50 MiB)、
  1 msサンプリングの最大private=37,494,784 bytes (35.76 MiB)。
  これは2 Graph・Planner・車両fixture・ランタイムを含み、1 Graphの上限との直接比較ではない。

追加テストは生成Visit課金、128超過の高頻度便、独立したWork/Memory/Services/Trips/
TotalVisits/Steps/Timingの制限、4,096同一OD packet共有と実積載、20,000 tickのOD、
短期間内の乗り換え解より後発直通の一般化費用が小さい反例、
後方の時刻不明と認証済みの早い最適解、Opaque停止と指令編集後の回復を検査する。
既存の環状/網状・普通/特急・複数幹線迂回・不確定混在・300乗り換え・
列車/道路/航空/船舶・予約/満員/部分積載・最終OD配送/支払い・
save/load・GUI・Shadow・無効時CargoDist一致も通過した。

決定性は冷/温キャッシュ同一Workと結果、ライブ入力の再読込、固定予算、
RNG不変、セーブと診断不変性で検査した。ライブサーバーのjoin/同期耐久試験は未実施。

実行例:

```powershell
cmake --build build-real-map-scaling --config Release --target openttd openttd_test --parallel 12
build-real-map-scaling/Release/openttd_test.exe '[passenger]' --reporter compact
ctest --test-dir build-real-map-scaling -C Release --output-on-failure -j 8
build-real-map-scaling/Release/openttd_test.exe 'Graph admission pays for generated visits rather than maximum trip reservations' --reporter compact
```

静的vcpkg依存を使う構成では `VCPKG_TARGET_TRIPLET=x64-windows-static` を指定して
ライブラリとMSVCランタイムを揃える。VSのReleaseプロダクションランナーには
ビルド生成の`lang`と`baseset`を実行ファイルの横にも用意する。

## 残る制約

不確定な暗黙停車、Depot/Slot/Counterなど未対応の指令、未設定の待ち時刻、
過ぎた旅行時刻でも到着していない車両、Full Load、Leave制約、
競合Scheduled Dispatch、Refitは引き続き保守的に扱う。
候補を一部しか予測できない状態をCompleteに昇格しない。
短期間が完全でも最大期間まで認証が必要なODでは、延長用のWork/メモリ/検索枠を
払えず延期することがある。失敗理由別にこれを観測できる。

今回の実マップでは検索・適用は0のままであり、第一目標は未達である。
時刻付き運行を追加した合成テストの成功を、その実マップの輸送成功に置き換えない。
指令/停車/時刻の安全な予測範囲を広げるには、現在の不確定な制御内容に応じた
別の認証実装または実マップの時刻表・運行指令修正が必要となる。
