# JourneyPlannerの再測定と比較基盤

最新の `jgrpp-ya`（`c2c4d1898c`）の再測定は完了した。[統合後の報告](passenger-routing-profile-c2c4d1898c-2026-10-13.md)と[JSON集計](measurements/passenger-routing-profile-2026-10-13/merged-c2c4d1898c/summary.json)を参照。環境変動を確認した先行6ラン、追加6ラン、独立した詳細1ランを保存し、追加6ランの基準値はoff **17.244** / on **32.347** / cold **52.115 ms/tick**。以下の未測定記録は初回の基盤整備時点のもの。

以下の測定未実施の記録は、初回の基盤整備時点（`a58d472382`）のもの。統合後の `604ce99389` を使う再測定では、同じ入力セーブと固定した資産を使用し、`--cache-off-scope all` で共有ODポリシー・次発インデックス・Fallback resident cacheを無効化する条件を追加した。`--wait-idle-seconds 3600 --idle-grace-seconds 90` は既存のビルド・テスト・ゲームを終了させずに待つ。測定中に競合が始まった場合は、こちらの測定だけを中断してそのランを除外する。

`604ce99389` の再測定は6ラン完走し、[測定報告](passenger-routing-profile-604ce99389-2026-10-13.md)と[JSON集計](measurements/passenger-routing-profile-2026-10-13/merged-604ce99389/summary.json)へ保存した。

ベースは `jgrpp-ya` の `a58d472382f85c63fe9114888d71d7d189c45572`、作業ブランチは `test/passenger-routing-profile`。通常車庫Wait修正を含む。製品ソース・経路選択・RNG・Work予算・保存形式の変更はない。変更はこの文書と `measurements/passenger-routing-profile-2026-10-13/` 内のスクリプト・診断証拠に限定した。

**現時点の実マップ測定は0ラン。** 最小計測版と詳細計測版を構築し、最終詳細カウンターを有効にした旅客回帰は **321 cases / 977976 assertions** 成功した。Pythonの証拠解析・比較条件・順位付け・観測ラッパーの検証は11件成功した。

最終測定開始チェックで別の `openttd` と `openttd-console` を検出したため、測定を開始しなかった。ユーザーのゲームや他Agentのゲームは終了させていない。入力・ビルドが欠けたための未測定ではない。**最新ベースライン、tick分位点、実マップのキャッシュ一致、CPU負荷による最適化順位は未取得**で、[JSON集計](measurements/passenger-routing-profile-2026-10-13/summary.json)と[順位JSON](measurements/passenger-routing-profile-2026-10-13/optimization-ranking.json)にも結果を補完していない。以前の33.772 ms/tickを今回のベースラインに転記しない。

測定前提とバイナリ識別は [verification-metadata.json](measurements/passenger-routing-profile-2026-10-13/verification-metadata.json)、回帰の生ログは [passenger-detail-tests.log](measurements/passenger-routing-profile-2026-10-13/passenger-detail-tests.log) に保存した。テスト中の合成マップの時間は性能比較に使わない。

## 入力と比較条件

前回の `verified-result.sav` がローカルにあり、SHA-256は `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390` と一致した。前報告上のマップは16384×4096、比較区間はStateTicks **3176545487 → 3176553679**。新しい実行でこの区間と8192 simulation ticksを確認するまで、その完走を主張しない。

前回の `run-resident` のbaseset/NewGRF等のリンク先worktreeは削除されており、前回の資産パッケージをそのまま再使用できない。現在のユーザーのOpenTTDデータから資産を複製し、今回のビルドのbaseset/langを重ねて固定した。247ファイル・693832662 bytes、資産一覧SHA-256は `b2b2322c16887cc25cd9d3d7e8cb85eebc366fd2721094d47baab6d40817827a`。今回の全条件・A/B比較で同じ固定パッケージを使う。前回の資産との完全一致、セーブが要求する全NewGRFの充足は未確認。ロード警告をstderrのハッシュと重複除去した全行で保存する。

コンパイラはMSVC 19.44.35227.0 x64、Release `/O2 /Ob2`、`/MT`、assert ON、LTO OFF、既存の静的vcpkgライブラリ。null video/sound/music。設定と起動は前回の [Measure-Residency.ps1](measurements/passenger-periodic-policy-residency-2026-10-13/Measure-Residency.ps1) を直接再使用する。旅客CargoDist automatic（distribution_pax=2）、OD assignment有効、recalc_interval=8、recalc_time=32。

| 条件名 | Journey Routing | periodic | 共有ODポリシーcache |
|---|---|---|---|
| off | 無効 | 無効 | 比較対象外 |
| on | 有効 | 有効 | 有効 |
| cold | 有効 | 有効 | 無効 |

`cold` は既存の `passenger_journey_stats cache_off` を使い、Timed/Scheduleの共有派生ODポリシーだけを無効化する。Untimed FallbackやTripのcacheをすべて無効化した結果とは呼ばない。これは前回の `-DisablePolicies` と同じ範囲。

既定値は各条件8192 ticks・2回。1回目はoff/on/cold、2回目はcold/on/offの順に逐次実行する。各回は同じ元セーブから始める。100msのメモリ・CPU観測、5秒ごとの競合プロセス確認を記録する。実行前にはゲーム・ビルド・テストがないことを確認し、実行中に見つかればそのランを無効として停止扱いにする。プロセス一覧が制限されるサンドボックスでは測定を拒否する。MSBuildの待機workerだけは除外する。検査のサンプリング間隔より短い競合実行を完全に検出する保証はない。

## 計測と包含関係

`Instrument-Profile.py` はconfigure済みVisual Studioプロジェクトを変更し、ビルドディレクトリに作成した翻訳単位のコピーを参照させる。製品ソースは変更しない。アンカーが変わった場合は停止するため、Agent A/Bの変更に応じて詳細計測位置を再確認できる。

`minimal` はconsoleのコピーへ既存1msヒストグラムのp50・bucket出力を追加する。シミュレーション中のタイマーやhot loopのカウンターを増やさない。既存の `passenger_journey_stats` の観測コストは含むので、診断を完全に除いた製品速度ではない。

`detail` は独立した翻訳単位のコピーに観測を加え、`detail-scopes.json` を出力する（旧ベース6個、次発インデックスを含む統合後7個）。ロード後のconsole `reset` で観測を開始する。統合後は各tickの時間サンプルと1024 ticksごとの累積カウンターも出力する。各区間の差分を集計するが、区間の番号だけで安定状態とは判定しない。結果は以下の範囲で利用できる。

| 項目 | 取得元・範囲 |
|---|---|
| Timed OD/NextDeparture回数 | `Periodic database.searches/next_departures`、detailの `timed.plan/next_departure.calls` |
| 逆向き下界cold構築・前向き探索 | `Periodic cache cost.compile_us`、detailの `timed.reverse_lower_bound/forward` |
| 位相走査・候補系統・label・支配・queue | detailの `timed.vehicle_phase_scan/visits`、`candidate_service_visits`、`label_attempts`、`dominance_checks`、`queue_push/pop` |
| Live Station参照 | コピーした経路関連5翻訳単位の `Station::Get/GetIfValid/IsValidID` 呼出しを `live.station_*` として観測。ゲーム全体のStation参照数ではない |
| Work/Label/Memory | `Periodic OD calls` の結果と重複観測、Fallbackの理由別延期、detailの前向き停止数とWork停止位置（queue/系統/位相/dispatch/降車/支配） |
| cache hit/miss/stale/eviction | 既存のPeriodic/Schedule診断。stale・evictionは共有LRU合計 |
| Schedule検索・待機推定 | 既存hit/miss/work/query_us、detailの `schedule.plan/search/wait_estimate` |
| Untimed Distance・binding・reverse・BFS・reuse | 既存Fallback診断とdetailの `fallback.distance/node_binding/node_binding_validation/node_binding_rebuild/reverse_index/bfs/cache_reuse`。reuseはresident distance hitの処理範囲 |
| 待機packet・intent・実車検証・移動再分類 | detailの `loading.waiting_scan/waiting_packet_visits/intent_current/intent_and_live_ride/live_vehicle_validation/packet_stage/packet_load/packet_append/waiting_reinsert` |
| 実乗車・乗換・最終配送 | 既存のJourney/Schedule/Untimed transport、Completed interchanges。計画採用と実輸送を区別 |
| メモリ | process peak Working Set、sampled peak private、DB/cache bytes、graph/queryの論理・保持見積もり |

時間は `steady_clock` によるシミュレーションスレッド上の経過時間で、OSのexclusive CPUサンプルではない。`timed.forward` は位相評価とNextDepartureを含み、`schedule.search` はwait_estimateを含む。DBの `build_us` は `Tick()` 全体で公開を含み、`publication_us` を再加算しない。waiting/boarding/fallbackも包含時間で、上位のJourneyや検索と足さない。BFSはloop部分、binding_validationはresident bindingの全件検証loop部分。前向きqueryからcold compileを引いたminimalの値はcache処理等も含むので、関数単体の時間と呼ばない。

詳細タイマーのコスト、特にNextDepartureの多数の呼出しのコストは測定値を変える。詳細ランを速度比較へ混ぜない。細かなCPU割合を主張するには、別のシンボル付きsampling結果で裏付ける。現時点ではsamplingも未実施。

tickのp50/p95/p99はnearest-rankの1ms bucket上端。2047番bucketはoverflowで有限の上端を持たないためnullを返す。最大値は既存の実測max_us。CPU秒・private memoryは前回ハーネスの100ms最終観測値、CPU秒にはロード・セーブ・worker threadも含み、simulation tick平均に換算しない。

## 再測定

WindowsのホストPowerShellで実行する。入力・資産・exe・最終セーブは `build*` 以下に保持し、Gitに追加しない。以下はこのworktreeに作成済みの固定パッケージを使う例。別のゲーム・ビルド・テストを終了させてから実行する。既存consoleハーネスの制約から、出力先は絶対パスに空白を含まない場所を使う。既に失敗したmanifestがあるディレクトリには上書きせず、新しい出力先を指定する。

```powershell
$taskSource = (Get-Location).Path
$taskTools = Join-Path $taskSource 'docs/measurements/passenger-routing-profile-2026-10-13'
$taskBuild = Join-Path $taskSource 'build-journey-profile'
$taskDependencies = 'C:/Users/bebe/CLionProjects/OpenTTD-patches/cmake-build-release-visual-studio/vcpkg_installed/x64-windows-static'
& "$taskTools/Build-Profile.ps1" -Source $taskSource -Build $taskBuild -Dependencies $taskDependencies -Mode minimal
python "$taskTools/Run-Profile.py" run --package "$taskBuild/package" --binary "$taskBuild/openttd-minimal.exe" --build $taskBuild --output "$taskBuild/baseline-completed" --label baseline --revision a58d472382f85c63fe9114888d71d7d189c45572
python "$taskTools/Summarize-Profile.py" "$taskBuild/baseline-completed/manifest.json" --output "$taskTools/summary.json"
python "$taskTools/Rank-Profile.py" "$taskTools/summary.json" --output "$taskTools/optimization-ranking.json"
```

別環境ではビルド後に固定パッケージを作る。

```powershell
python "$taskTools/Run-Profile.py" freeze --package "$taskBuild/package-new" --save 'PATH/verified-result.sav' --config 'PATH/openttd.cfg' --assets 'PATH/OpenTTD-data' --build $taskBuild
```

`--save` の既定検証は前記SHA-256。主測定用入力を別セーブへ暗黙に置換しない。別区間の検証用には `--allow-different-input` を明示し、同じcheckpointを全比較バイナリへ渡す。初期構築と安定区間を追加する場合、全バイナリで同じ保存済みcheckpointを共有し、`run --phase` を別名にする。periodic/onと前記設定で保存したcheckpointを続行するときは `--resume-settings --modes on cold` で設定の再適用を避ける。offはこの保存済み設定をそのまま使えないため併用を拒否する。現時点では8192 ticks内の安定区間を判定した観測はない。

詳細版はminimalの6ランが終了した後に構築・実行する。

```powershell
& "$taskTools/Build-Profile.ps1" -Source $taskSource -Build $taskBuild -Dependencies $taskDependencies -Mode detail
python "$taskTools/Run-Profile.py" run --package "$taskBuild/package" --binary "$taskBuild/openttd-detail.exe" --build $taskBuild --output "$taskBuild/detail-completed" --label baseline --revision a58d472382f85c63fe9114888d71d7d189c45572 --kind detail --modes on --repetitions 1
```

観測を有効にした既存回帰の実行例（性能測定とは別に実行する）：

```powershell
python "$taskTools/Enable-Test-Observer.py" $taskBuild
cmake --build $taskBuild --config Release --target openttd_test --parallel 8
& "$taskBuild/Release/openttd_test.exe" '[passenger]~[.benchmark]' --reporter compact
python "$taskTools/Test-Profile.py"
```

## Agent A/Bとの比較

A/Bのブランチも同じDependenciesでminimalを構築し、同じ固定packageを使って `--label agent-a` / `--label agent-b` と実際の完全なコミットSHAで実行する。生成情報にはソース差分ハッシュも保存する。詳細計測のアンカーが変わる場合、位置を確認してから独立コピー側を適合させる。minimalの比較はconsole出力追加だけを共通にする。

```powershell
python "$taskTools/Summarize-Profile.py" 'BASELINE/manifest.json' 'AGENT-A/manifest.json' 'AGENT-B/manifest.json' 'DETAIL/manifest.json' --output "$taskTools/summary.json"
```

入力、全資産一覧、config、コンパイラとライブラリ、ホストCPU、設定、StateTicks区間、ハーネス、phaseが一致する場合だけ速度差を出す。detailは比較から除外する。各ランの平均とそのばらつきを保持し、反復の差を信頼区間とは呼ばない。Journey on/coldは最終セーブSHA-256、Work、結果・延期理由、実輸送を比較する。offとのゲーム状態一致は要求しない。A/Bとベースの輸送・Work差も別に表示する。

`Rank-Profile.py` は有効なminimal/onの観測から、Timed前向き＋cacheその他、逆cold構築、Schedule検索、DB tick（構築・公開を含む）、残るJourneyの経過時間を順位付けする。NextDepartureやODの呼出し数だけからCPU負荷の順位を推測しない。実測0ランの現在は `not_measured`。最新の負荷に基づく順位の確定は測定後の作業として残る。
