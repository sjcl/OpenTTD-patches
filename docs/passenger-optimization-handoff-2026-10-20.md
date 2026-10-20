# Passenger Hyperpath 最適化の引き継ぎ（2026-10-20）

Claude（claude.ai のクラウドセッション）から Claude Code セッションへの引き継ぎ資料。
対象ブランチは `sjcl/OpenTTD-patches` の `merge/hyperpath-with-table-build`。最新は `55f1cfe0`。

## 1. 目的と進め方

- 目的は、Hyperpath を有効にした旅客経路処理の高速化。対象は実際に最適化したい実セーブ `gc3833d807.sav` の通常 tick。
- 厳密に同一の結果までは求めない（ユーザー指示）。ただし次の 3 点は必須。
  - 決定性
  - マルチプレイの同期（異なるコンパイラ・worker 数でも desync しない）
  - セーブ互換
- **これまでの最適化はすべて、結果を変えないものとして作った。** 計測の canonical 24 項目（packet・DB・所属・積載・支払いの digest、配送数、公開行数、総 Work など）が前の版と完全一致することを合格条件にしている。
- 作業の分担。
  - 実装・テスト・push は AI 側。
  - 実マップ計測はユーザー側（Windows、Ryzen 9 9950X）。別のエージェントが計測し、結果を `docs/` と `docs/measurements/` に push する。
  - ユーザーの「計測しました」を合図に fetch して解析し、次へ進む。会話は日本語。
- 計測の手順と採用基準は **[計測手順書](passenger-wpr-runbook.md)** と [AGENTS.md](../AGENTS.md) に従う。canonical の一致と validate の合格は別々に判定する。

## 2. 現在の状態と最優先タスク

### 0d99a67f の canonical 不一致と、その修正 55f1cfe0（未検証）

- **何が起きたか**: [0d99a67f の ABBA](passenger-wpr-abba-0d99a67f-2026-10-20.md) で、Hyperpath の canonical が 24 項目中 11 項目ずれた。Legacy は一致していた。validate のカウンター（memo・lookup の violations、lookup_duplicates）はすべて 0 だった。
- **特定した原因**: a968222f で入れた「表の `Board` 結果の共有キャッシュ」。
  - `PassengerPrecomputedTable::Select` は、ride の `signature`/`generation` を表の **member binding**（`refreshed_members`）から取る。
  - この binding は、時刻表だけの変更で再公開されたとき（`RefreshService`）に更新される。グラフ・行・lookup epoch はどれも変わらない。
  - そのためキャッシュに残った ride が古い generation を持ったままになり、`IsCurrent` の判定が変わった。
  - Legacy で一致したことは、この経路が Hyperpath の表だけで共有されることと整合する。
- **修正（55f1cfe0）**: 表に binding 更新の回数 `member_refreshes`（`MemberRefreshes()`）を持たせた。`Board` キャッシュの slot はこの回数も照合する。古い意図の計画の共有（`SharedHyperpathPlan`）も同じ回数に追従させた。再現テスト「Cached Hyperpath boarding follows a refreshed carrier binding」を追加し、回数照合を外すとこのテストが失敗することも確認した。
- **次にやること**:
  1. ユーザーに依頼して、6c11c67f と 55f1cfe0 を ABBA で計測し、validate も 1 本採る。**最初の判定は canonical が 6c11c67f と一致するか。**
  2. まだずれる場合は、a968222f と 0d99a67f を個別にビルドして二分探索する。疑う順は次のとおり。
     - 表の可変状態のうち、行・epoch 以外のもの（`refreshed_members` のほかにもないか）。
     - `CachedHyperpathMembership` の行キャッシュ。
     - `MayUseHyperpathPolicy` から context memo を外したこと。
  3. 一致したら、性能を評価する。0d99a67f の B1（1 本だけで、canonical もずれている）では、次のように大きく下がっていた。修正後の ABBA で確定させる。
     - `MayUseHyperpathPolicy`: 0.52 → 0.27 ms/tick
     - `SharedHyperpathPlan` の self: 0.053 → 0.003 ms/tick

## 3. 計測の基準（2026-10-19 から）

- **リリース構成**: LLVM 23.1.3 の ThinLTO（clang-cl/LLD）。ユーザーの [Windows 既定ビルド](windows-default-build-2026-10-19.md)。
- **性能の A/B**: ThinLTO、**assert OFF**、**PGO OFF**、静的 CRT、`/O2 /Ob2 /MT /Z7`。差が小さいので、必ず **A→B→B→A** の交互計測にする。
- 1 本ずつの比較は、machine factor の補正があっても信用できない（同じコードで 24% 振れた回がある）。
- 旧系列（MSVC 19.44、assert ON、LTO OFF）の数値とは、絶対値で比べない。
- 正しさの確認は validate モード（`--hyperpath-filter-mode validate`）と canonical で行う。validate のカウンターは次の 4 つ。
  - `Passenger hyperpath lookup: ... violations=`
  - `Passenger hyperpath memo: ... conservative=; violations=; lookup_duplicates=`
  - `conservative` は正常。退避された failed-carrier の方策が unknown 扱いになり、保守的に通っただけ。
  - 合格は `violations=0` かつ `lookup_duplicates=0`。
- 診断の出力は `passenger_journey_stats total`。worker が仕上げた行は `Precomputed parallel rows: ... finished_rows=` に出る。

## 4. 仕組みの要点（触る前に読むこと）

### 決定性の規則

- シミュレーションの判断に使ってよいのは、**保存されるフィールドだけ**。
- **派生キャッシュは判断を変えてはならない。** ヒットしてもミスしても、退避されても、同じ答えを返すこと。
- 乗車の判断は二段構え。
  - `MayUseHyperpathPolicy`: 保守的な事前フィルター（本来より多めに通す）。
  - `CanUseHyperpathPolicy`: FIFO の先頭で行う正確な判定。
  - 包含関係は `CanUse ⊆ MayUse ⊆ lookup 候補`。lookup の正しさは validate で検査する。
- worker と直列で、Work の消費量を完全に一致させる。worker が仕上げた行は、表全体への影響（到達可能な目的地の追加、乗車 mask の bit）を **同じ Work 位置で** 再生する（c1e30771）。どの slice 境界でも、公開状態が直列版と一致する。

### 派生キャッシュの無効化に使う鍵

| 鍵 | 変わる時 |
| --- | --- |
| `hyperpath_lookup_epoch` | 世代の開始・破棄、retained の実際の解放、ロード時の再生 |
| `hyperpath_destination_changed[destination]`（55f1cfe0 で導入） | その目的地の行が公開された時と、retained 行が解放された時（`BumpHyperpathDestination`） |
| `PassengerPrecomputedTable::MemberRefreshes()` | 時刻表だけの再公開で member binding が更新された時（55f1cfe0） |
| `BoardingRevision()` | `published_revision`、`boarding_revision`、`precomputed_operations`。表の構築中は毎 tick 変わる |
| `RevisionAffectsWaitingPlans()` | Legacy の表が存在する間は true。Legacy の ride は service（`unsafe_lookup`、OrderList の generation）にも依存するので、tick をまたいで共有しない |

### 主なコード

- `src/passenger_precomputed.{h,cpp}`: 事前計算表。
  - `GraphSnapshot`（凍結グラフ）と `DestinationWorkspace`。
  - `StepHyperpath`、`FinishHyperpathRow`、`PrepareHyperpathFinish`。
  - worker pool の `Run`、`AdvanceSearch`（結果の採用）。
  - `Plan`、`Board`、`Select`。
  - `LocalPolicy` と `StationChoices`（上限付きの派生キャッシュ）、`MarkBoardingCandidate`、`RefreshService`。
- `src/passenger_service_database.{h,cpp}`: DB。
  - `PlanPrecomputed`、`BoardHyperpath`、`MayBoardHyperpath`、`CachedHyperpathMembership`、`SharedHyperpathPlan`。
  - 直接写像の行キャッシュ `RowSlot`（plan、board、membership）。
  - `HyperpathLookupCandidates`、`TickPrecomputed`（`published` コールバック）。
- `src/cargopacket.{h,cpp}`: `StationCargoList` の待機索引。
  - Hyperpath の方策グループ `HyperpathWaiting`。memo 用フィールドを持つ。
  - occurrence → グループの逆引き `HyperpathLookup`。
  - `VisitBoardingCandidates`（walk、再利用 scratch、カーソル heap）、`RecordCapacityMiss`、`RefreshWaitingPacket`。
- `src/passenger_journey_active.{h,cpp}`: `PassengerJourneyLoadingContext`。
  - 生存期間は、駅ごと tick ごとの積み込み 1 回分。
  - `MayUse`/`CanUse`、`ResolveHyperpath`、`RetiredPlan`、`HyperpathMemoSerial`、`RefreshBoardingRevision`。
  - 待機イベント `RoutePassengerJourneys` と generation sweep。
- `src/economy.cpp`: `LoadUnloadStation` が context を 1 つ作る。`LoadUnloadVehicle` は**車両（各車）ごと**に `Load` を呼ぶ。
- テスト: `src/tests/passenger_service_database.cpp`、`src/tests/passenger_precomputed.cpp`。test access は `src/tests/passenger_async_access.h`。

## 5. これまでの最適化（新しい順）

| commit | 内容 | 実マップの結果 |
| --- | --- | --- |
| 55f1cfe0 | `Board`/plan キャッシュを binding 更新に追従させる修正 | 未計測 |
| 0d99a67f | 乗り継ぎグループの保存済み判定を、DB の直接写像キャッシュで tick 間共有。context memo は廃止 | canonical 不一致（原因は a968222f 側。上記） |
| a968222f | 共有キャッシュを固定サイズの直接写像に変更し、表の `Board` 結果も共有 | 同上 |
| 6c11c67f / b5bab2fd / c1e30771 | 行の仕上げを worker に移し、Work 位置で再生。古い意図の計画の同一性を共有 | `StepImpl` self −95%、主スレッドの `TickPrecomputed` −0.29 ms/tick（[ABBA](passenger-wpr-abba-6c11c67f-2026-10-19.md)） |
| f97bac71 | jgrpp-ya の LLVM 23 ThinLTO 既定ビルドを merge | 新しい基準（[計測](passenger-wpr-clang-thin-merged-2026-10-19.md)） |
| 8f829b8c | 積み込み walk の scratch 再利用、context の遅延構築、古い意図の計画を context 内で共有 | Load 配下の malloc −38〜55% |
| 6aeaff68 | validate で、保守的な不一致と本当の違反を区別 | [実マップ validate 合格](passenger-hyperpath-validate-6aeaff68-2026-10-19.md) |
| 0b8a21c0 | 同じ編成の各車で事前フィルターの答えを再利用し、行の作業バッファの捨て直しをやめた | Hyperpath −0.80 ms/tick（補正後）。`StepHyperpath` −43% |
| 97cdc0ad | 待機 packet の索引をその場で更新 | Hyperpath −0.67、Legacy −0.30 ms/tick |
| 9ab0228d | `LocalPolicy` で `StationChoices` を共有 | `LocalPolicy` −84% |
| 8ff4834d / f9c4a85b | generation sweep を時間で抑制し、無効化をまとめて扱う | Legacy の滞留が解消 |
| 8462e6cd | Hyperpath の逆引き lookup を tick 間で保持 | 全体再構築 −89% |

## 6. 残っている重い所（6c11c67f の Clang B、Hyperpath、主スレッド、ms/tick）

- `StationCargoList::Load` 1.57
  - walk 0.72。うち `MayUseHyperpathPolicy` 0.61（0d99a67f 以降は 0.27 の見込み）。
  - FIFO 先頭の `ResolveHyperpath`/`Board` 0.21〜0.27（a968222f で共有済み、効果は未確認）。
  - 積み込み操作 0.33。
- `RoutePassengerJourneys` 0.81
  - `RefreshWaitingPacket` 0.38、`PlanWaiting` 0.18、`ForgetWaitingPacket` 0.13。
  - packet ごとの原因別タイマー（`steady_clock::now`）で約 0.04。
- `RecordCapacityMiss` 0.24。
- `RefreshHyperpathLookup` 0.23: 大半はロード直後に各駅で最初に作る分（`rebuilds` 約 2,700 / 2,048 tick）。定常状態では小さいはず。
- heap: `std::set`/`map` のノード操作（待機索引、context）が多い。
  - MSVC STL は clang-cl でも同じで、**map/set は既定コンストラクタでもメモリを確保する**。
  - pool allocator（`std::pmr`）は候補。
- Legacy モード: 保存された Hyperpath 意図を Legacy の表で計画し直すコスト（`RetiredPlan` 約 0.41）。モードを切り替えた直後の過渡状態に特有で、Legacy の表がある間は共有キャッシュを使えない。

## 7. 未着手の案

- 行の公開に合わせて、その目的地の待機 packet だけを再計画する（sweep を目的地単位に絞る）。挙動が変わるので、canonical の再基準化が必要。
- 待機索引のノード系コンテナを pool 化する。
- 原因別タイマーをサンプリングにする（診断値の意味が変わる）。
- radix キューとリング型キューを Clang で比べ直す。以前は GCC では速く、MSVC では遅かった。
- tick をまたぐグループ memo。OrderList の状態や設定にも依存するので危険で、やっていない。

## 8. ハマりどころ

- Windows SDK の `far` マクロ（キューの変数名で衝突した）。新しい名前は MSVC/clang-cl でも通るか確認する。
- 派生キャッシュを作るときは、**入力のうち保存されない可変状態をすべて洗い出す**。0d99a67f の不一致はこれで起きた。
- `LocalPolicy` は 4096 件の上限で退避される。そのため事前フィルターの答えは「unknown → 保守的に通す」ことがあり、validate ではこれを `conservative` として数える。
- テストは cross-mode 等価、sliced 構築、serial/parallel の公開一致の 3 系統が強い。**キャッシュや再生の変更では、わざと壊した改変（mutation）で、テストがそれを検出することを必ず確かめる。**
- 表構築の digest ベンチ: `openttd_test "[.hyperpath-build-cpu]"`。期待値は hyperpath `75f6c7b724013583`、legacy `575548db5660677a`。表の中身を変えない変更では不変であること。
- Linux の参考ビルドは GCC、Release、assert ON、`ninja openttd_test openttd`。全テストは 782 件。clang 18 による構文チェックも使った（Windows のビルドは clang-cl 23）。

## 9. 計測記録の索引

`docs/passenger-wpr-*.md`（2026-10-18 以降が新しい手順）、[手順書](passenger-wpr-runbook.md)、[コンパイラ比較](compiler-lto-real-map-2026-10-19.md)、[ThinLTO+PGO](clang-thin-pgo-2026-10-19.md)。
各記録の `docs/measurements/<name>/` に、関数別 CSV と生の WPA stacks（gzip）がある。
主スレッドの呼び出し木は、stacks CSV から `StateGameLoop` を含むスタックだけを集計して作った。親子の inclusive を二重に数えないこと。
