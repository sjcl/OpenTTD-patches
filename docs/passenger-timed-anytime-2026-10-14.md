# Timed JourneyのAnytime採用

ベースは `c2c4d1898c3a38a091ff224c5be660ede72238f7`、ブランチは `feat/passenger-timed-anytime`。
観測だけを追加した `3f66b40dc0` のバイナリを先に固定し、旧pop-only方式の実マップ診断と、Anytime採用後の実マップ測定を分ける。
既存の比較基準は [632399cfc1の8,192tick報告](https://github.com/sjcl/OpenTTD-patches/blob/632399cfc1e6c0c9125a58e1f2c9a122175f79ba/docs/passenger-routing-profile-c2c4d1898c-2026-10-13.md)。

## 探索結果と採用

`PassengerPeriodicPlan::state` は検索結果だけに保持する。`Reason` は打ち切り理由、`observations` は重複し得る観測理由として引き続き使う。

| state | 意味 | Timed経路採用 |
|---|---|---|
| Certified | 従来の下界・省略候補判定で、公開されたTimed探索領域の最良完全経路を確認 | 可 |
| Provisional | 完全経路を保持しているが、同費用のタイブレークを含め、残る探索から改善できないことを未証明 | 可 |
| Deferred | Work・Label・Memory・乗換安全上限で、有効な完全経路を返せない | 不可 |
| NoRoute | 所定の公開Timed探索領域で経路なしを証明 | 不可、Schedule-aware/Untimedは別途判定 |
| Unavailable | 無効な問い合わせ、未対応評価、未知の位相等で結論を出せない | 不可 |

Provisionalの`deferred`はtrueのままとし、`Reason::Found`には変更しない。既に目的地Labelをpopしたが探索を証明まで完了できなかった従来の結果もProvisionalに分類する。新たに救済できた検索は、その従来のpop済み候補とは区別して `rescued_unpopped` に数える。

Label挿入は、既存の支配判定とWork/Label/Memory admission、Label格納、完全候補の記録、queue挿入の順。
候補のキーは費用、到着時刻、乗車回数、最初の発車時刻、ServiceID、乗車Order Index、降車Order Index。
同じキーでは最初に格納された候補を保つ。`incumbent_cost`による既存の枝刈りや、queueをpopするまでのWork・探索順序は変えない。

返却前に、目的地Labelから親indexが厳密に減少するチェーンをたどる。区間の乗降駅、乗車回数、時刻、Serviceのincarnation/generation、Origin/readyのroot、最初の区間との一致を確認する。
部分Label、admissionで拒否したLabel、親チェーンを復元できない候補を採用しない。

`PlanWaiting()`は検索未完了とpacket採用を別に扱う。イベント内cacheはProvisionalと未完了フラグを保持するが、rideがあれば通常のTimed ServiceIntentを採用する。経路がなければ従来どおりSchedule-awareを残りWorkで試し、必要ならpacketを延期する。

## 実乗車と保存

CargoPacketへ保存するのは従来と同じ、次の一乗車区間だけ。Provisional状態や完全な旅程を保存しない。CargoPacketの形式・サイズ、JPSDの保存version、ネットワーク同期形式は変更しない。

実際の予約・乗車・継続は `ValidatePassengerServiceRide()` と既存のCargo移動処理を通る。Serviceのincarnation/generation、駅とOrder Occurrence、CargoType別NoLoad/NoUnload、途中の強制降車、最低乗換時間、車両容量・実空席・予約、車庫・Waypoint・運転停車、遅延中の実車の扱いを変更しない。
乗換後は実際の現在時刻と到着Serviceから再計画する。Provisionalの先行旅程はその先の乗換や座席を保証しない。

採用する候補は整数費用と保存されたIDの順で決まる。壁時計、診断カウンター、policy/次発index/Scratchの常駐状態は判断に使わない。旧方式は `PlanWithLegacyCompletion()` に残し、ローカル設定で製品アルゴリズムを切り替えられるようにはしない。

## 診断の範囲

`passenger_journey_stats total` に、最終ReasonがWorkの検索について完全Labelの有無、queue残存・pop済み候補の有無、改善可能な未探索下界、逆探索と前向きの論理Workを追加した。
queueは読み取るだけで、並び替えたりpopしたりしない。最良候補の費用・到着時刻は各Planの`search`に保持し、consoleには最大16検索の例を出す。

停止地点の`calls`は、その地点で止まったOD数。`observations`は同じOD内の繰返しを含む。複数地点のcallsやobservationsを足してOD数とは呼ばない。
`remaining_lower_bound <= best_complete_cost` は費用だけでなく同費用の改善を排除できない場合も含む。
Workのp95は1024単位の上端を用い、今回の製品問い合わせの上限は65,536。逆探索cache hitもcoldと同じ論理Workを再現する。

## 回帰と比較

Work 0〜65,536の全境界を旧pop-only方式と比較し、論理Work、Label、Memory、停止地点の一致を確認する。旧Certifiedの経路も一致を要求する。
完全Label生成直後、pop直前、目的地なし、より良いsuffixが未探索、同費用候補、Label/Memory admission、未知位相とNoRouteの区別を個別に確認する。

通常の65,536Workで実際に未popの完全経路を採用するfixtureは、64往復停車×80両の4サービスと直接サービス・接続サービスからなる。各サービスはDB公開の上限内に収める。
待機packetとイベント内cache、CargoPacket保存・復元、cache無効のpeerからの同一結果、遅延実車への予約を確認する。
既存の旅客回帰も通し、0/1/多段乗換、普通/特急と発車位相、車庫・繰返し停車・強制降車、マルチモード、Schedule-aware、Untimedを検証する。

実マップの品質比較は、別バイナリで1024問い合わせごとに同じOD・cargo・ready・除外Service・ライブ状態の旧方式と高Work参照を実行する。
高Work参照はWork 1,048,576、Labels 65,536、Memory 128 MiB。参照がCertifiedの場合だけ最適費用・到着時刻との差に含める。
系統的な標本であり、全ODの品質保証とは扱わない。追加探索のCPU負荷があるため、このバイナリの時間は主性能比較に入れない。

## 実マップ条件と再現

入力Save SHA-256 `14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`、config `bb9f97ff8504cbb287ef0976add7a33cb27440f5ecd9ccf4febf0aa4746fa122`、247資産の一覧 `339d61fef7c442e3d40f1007d48c2544484e38406e11263003cfce6238f97d42` を固定する。
参照元worktreeの削除により最初の試行はゲーム起動前に失敗した。ハッシュが一致するローカルファイルだけから、このworktree内へ独立コピーを復元した。失敗した試行を実測として採用しない。

MSVC x64 Release、`/O2 /Ob2 /MT`、assert ON、LTO OFF、null video/sound/music。
他のビルド・テスト・ゲームとの競合を共有ロックとプロセス監視で避け、起動前に90秒の静穏を確認する。検出された競合ランは速度比較から除外する。
Save・exe・NewGRFはGitへ保存せず、診断、console、ハッシュと検証結果を[測定資料](measurements/passenger-timed-anytime-2026-10-14/)へ保存する。

```powershell
# 依存関係は既存測定と同じ静的vcpkg一式を使う。
pwsh -NoProfile -File docs/measurements/passenger-routing-profile-2026-10-13/Build-Profile.ps1 -Source . -Build build-timed-anytime -Dependencies PATH/TO/x64-windows-static -Mode minimal
python docs/measurements/passenger-routing-profile-2026-10-13/Run-Profile.py run --package PATH/TO/FROZEN-PACKAGE --binary build-timed-anytime/openttd-minimal.exe --build build-timed-anytime --output PATH/TO/NEW-OUTPUT --label anytime --revision FULL_REVISION --modes on cold --repetitions 2 --ticks 8192 --cache-off-scope all --wait-idle-seconds 3600 --idle-grace-seconds 90
python docs/measurements/passenger-timed-anytime-2026-10-14/Summarize-Anytime.py PATH/TO/MANIFEST.json --output PATH/TO/NEW-ARCHIVE
# 品質版: 再configure → Instrument-Profile.py --mode minimal → Prepare-Quality.py → openttd build。
python docs/measurements/passenger-timed-anytime-2026-10-14/Prepare-Quality.py . build-timed-anytime --stride 1024
python docs/measurements/passenger-timed-anytime-2026-10-14/Summarize-Quality.py PATH/TO/anytime-quality.jsonl --output PATH/TO/quality-summary.json
```

## 採用変更前の実マップ診断

8,192 ticks、131,070検索の基準診断は、論理Work・実輸送・最終Saveが既存測定と一致した。最終Save SHA-256は `c8d4323412fdf2e85e53d30eda98d7815a639691a62f25f47b2cfeb3c8c7f2a9`。

| Work最終終了の検索 | 件数 |
|---|---:|
| 完全候補あり | 37,595 |
| 完全候補なし | 28,831 |
| 完全候補がqueueに残存 | 37,595 |
| pop済み完全候補あり | 0 |
| 最良候補の改善を排除できない下界が残存 | 37,595 |

完全候補を保持する割合はWork終了の **56.60%**、全Timed検索の **28.68%**。これは救済可能性の診断値であり、変更後の実採用や最終配送数とは区別する。
Work終了検索で消費した逆探索Workは880,957,611、前向きWorkは3,472,336,725。各検索の両者の和は65,536。
最良候補の費用・到着時刻の例と停止地点の全カウンターは[基準診断](measurements/passenger-timed-anytime-2026-10-14/baseline/summary.json)に保存した。

この最初の診断版は、停止地点の下界を各Work消費時にも計算していたため、simulation平均42.873 msの観測負荷を含む。主性能比較には使わない。
製品では失敗したWork消費時だけ下界を評価する。速度対照は `Prepare-Legacy.py` による旧pop-only方式とし、Anytimeと同じ軽い診断・復元検証を使う。

変更後の測定値・最終検証ログは収集後に追記する。
