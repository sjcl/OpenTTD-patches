# Passenger 実マップ・WPR計測手順

最終更新: 2026-10-19。WindowsでLegacy / Hyperpathを同じセーブから各2,048 tick測るための手順。
[0b8a21c0の計測](passenger-wpr-coach-memo-0b8a21c0-2026-10-19.md)で、欠落のない新旧4本を取得できた方法を基準にしている。
この文書へは、毎回読むリポジトリ直下の [AGENTS.md](../AGENTS.md) からリンクしている。

**最初に短いpilotで採取方法を確かめ、各本の品質を確認してから次へ進む。CSV出力や集計だけの失敗なら、検証済みETLから後処理を再開する。**

## 1. 着手時に確定するもの

今回の作業ディレクトリに次を記録してから始める。以前のチャットや消えるbuildディレクトリだけを根拠にしない。

- 対象の完全なcommit ID、比較基準のcommitとGit追跡済みレポート、対象モード、tick数、注目関数。
- 固定セーブ・cfg・アセットの保管先とSHA-256、開始tick、予定終了tick。既存の固定入力があるなら再利用する。
- 通常条件か、定常化後か、再構築条件か。結果のcanonicalが同一になるという要求があるか。
- コンパイラ、CMakeオプション、計測用コピーの変更内容、exe/PDBと言語ファイル、WPR/WPAの設定とバージョン。
- 生ETLと入力・exe/PDBを作業ツリー外に保管する場所、空き容量、競合ゲーム・ビルド・テストの有無。

今回のrun IDを含む新しい作業ディレクトリを使う。既存の計測ディレクトリを上書きしたり、以前の合図ファイルを再利用したりしない。
旧版のexe/PDBはビルド前に別の場所へ保存し、過去の `build-identity.json` とハッシュが一致することを確認する。
共有コンパイル先の `compile/Release` は次のビルドで変わるため、保存済み基準バイナリと取り違えない。

### 基準入力 gc3833d807.sav

以下は既存の比較系列の識別情報で、新しいセーブへ一般化する値ではない。

| 項目 | 値 |
| --- | --- |
| 元セーブ | `C:\Users\bebe\OneDrive\ドキュメント\OpenTTD\save\gc3833d807.sav` |
| セーブ容量 | 46,620,248 bytes |
| セーブSHA-256 | `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a` |
| cfg SHA-256 | `bf8503ba53c6ec79dd57380a2244aa6b77014663ccfc1b53205ce1e09d94f30c` |
| package.json SHA-256 | `11d72b48390395cb6283d365efcc2e960557e301ab253fcc8b44c5bdee2fa4b7` |
| アセット数 | 245 |
| StateTicks | 3,197,276,448 → 3,197,278,496（2,048 tick） |

照合元は [input-identity.json](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/input-identity.json) と [固定入力manifest](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/input/package.json)。
生ETL・exe/PDBの保存先は [recording-backup.json](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/recording-backup.json)、固定入力ZIPの保存先は [input-backup.json](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/input-backup.json) からたどる。
パスの存在だけで判断せず、復元したファイルをハッシュ照合する。

既存入力との比較では、元のセーブから `input.sav` をコピーし、cfgと全アセットを既存manifestに照合する。セーブを変換・再保存しない。
セーブ・cfg・manifestは独立したコピーにし、凍結セーブは読み取り専用にする。変更しないアセットやWindowsのsymbol cacheは共有・hardlinkを利用できる。
初めての入力には `utils/passenger_precomputed_real_map.py freeze SAVE ASSETS BUILD PACKAGE` が使えるが、既存の比較系列でcfgやアセットを作り直す理由にはしない。

定常状態を新たに作る場合だけ、指定モードで8,192 tick等の指定区間を走らせ、保存・凍結・再ロード検証する。
新しい入力から比較基準も取り直す。Hyperpathで作った定常セーブをLegacyへ切り替えると保存済み意図の処理が残る場合があるため、共通入力の比較とモード別定常入力の比較を分けて記録する。

## 2. 計測用ビルドを準備する

製品ソースのcommitを固定し、計測フックはコピーしたtranslation unitにだけ挿入する。
参考の [ビルドレシピ](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/build-command.json) はMSVC 19.44 / VS 2022 x64 Release、静的CRT、assert ON、LTO OFF。
比較対象と次の設定をそろえる。

```text
-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
-DOPTION_USE_ASSERTS=ON
-DOPTION_NO_LTO=ON
-DOPTION_DEDICATED=OFF
-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi /Zo
-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi
-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO
```

基本の順序は CMake configure → `passenger_estimated_prepare.py SOURCE BUILD REV --cpu` → `passenger_wpr_prepare.py BUILD` → build。
生成済みプロジェクトへコピーTUを向ける処理なので、フック挿入後に再configureしたら再度prepareが必要。prepareの重ね掛けもしない。
buildは `cmake --build BUILD --config Release --target openttd_test openttd --parallel 12 -- /nodeReuse:false`、nativeテストは `openttd_test.exe --reporter compact`。
ビルド・テストは採取開始前に終わらせる。

**prepareのアンカーを先に確認する。** 0b8a21c0では `wait_saved_replay` 追加によりタイマーのアンカーが2か所になり、従来のprepareが失敗した。
[prepare_compat.py](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/checks/prepare_compat.py) は、計測スクリプトのコピー側で `bool ready = false;` を含むアンカーへ限定した例。
`while (!db.PreparePrecomputed()) {}` の挿入先は `build_precomputed` の中だけにする。別のタイマーへ挿入しない。
以後の版でもアンカーと生成差分を確認し、変更時は元・コピーのハッシュと理由を残す。製品コードの挙動を計測の都合で変えない。

ビルド完了後、exe/PDB・言語ファイル・baseset・生成した計測TU・レシピを今回の保存先へ取り出す。
対象commit、`src` tree ID、exe/PDBのSHA-256を `build-identity.json` に残す。対応しないPDBや言語ファイルを混ぜない。
起動環境から以前の `OPENTTD_*` を除き、今回のrunnerに必要なものだけを設定させる。古いtrace/probe設定を持ち込まない。

## 3. WPRの短いpilotを先に通す

採取には [utils/passenger_wpr_cpu.wprp](../utils/passenger_wpr_cpu.wprp) を使う。
これは成功した [cpu-minimal.wprp](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/cpu-minimal.wprp) と同じバイト列。
SHA-256は `e0a89dab28d5379469aa3ad23f65e8c87256014951065dae14b9429998251a82`。

- File方式。system bufferは1,024 KiB × 1,024個。
- kernelの `CpuConfig`・`Loader`・`ProcessThread`・`SampledProfile`、および `SampledProfile` stackのみ。
- **EventCollector / EventProviderは含めない。** CSwitch等を外すだけでuser event collectorが残っているプロファイルとは異なる。
- `wpr -profint` の値を記録する。今回の基準は10000（1ms）。異なる場合に黙ってシステム設定を変えない。

通常の `wpr -start CPU` のMemory方式では開始マーカーが上書きされ、標準CPUのFile方式では大きなイベント欠落が起きた。
バッファ増量だけでも解消しなかった。最初から上記の検証済み設定を使う。
この設定はCPUサンプリング用であり、CSwitchや待機時間の分析が必要な別課題では別採取を設計する。

新しい出力先で、管理者のrecorderから以下の短い試験を行う。`PROFILE` / `TEMP` / `PILOT` / `XPERF` は今回の絶対パスに置換する。

```text
wpr -start "PROFILE!CPU" -filemode -recordtempto "TEMP"
（1秒待つ）
wpr -marker pilot-begin -flush
（3秒ほどCPUを動かす）
wpr -marker pilot-end -flush
wpr -stop "PILOT/cpu.etl"
XPERF -i "PILOT/cpu.etl" -o "PILOT/trace-stats.txt" -a tracestats -timespan
XPERF -i "PILOT/cpu.etl" -a marks -summary -ao "PILOT/markers.txt"
```

全コマンドの終了コード0、`Total # Lost Events` と `Total # Lost Buffers` の両方0、両マーカーあり、begin < endを確認する。
**pilotが通るまで実マップの採取を始めない。** 本番でも各本を同じ条件で判定する。
欠落0だけではMemory ringの履歴保存を証明できないため、マーカー検査を省略しない。
採取プロファイルや間隔を変えた場合は比較基準も同条件で採取し、旧方式の値は参考値として扱う。

## 4. 1つのrecorderで直列に採取する

WPR用に1つの管理者recorderを起動し、pilotと必要な全ジョブを順番に処理する。ゲーム本体は通常権限で実行する。
別のWPRセッションが動いていたら用途を確認し、自分が開始していないセッションをcancel/stopしない。
UACの起動には非表示ウィンドウを使う（`Start-Process ... -Verb RunAs -WindowStyle Hidden`）。開始失敗や取消しを成功扱いしない。

再利用の参考は成功した [record_minimal.ps1](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/checks/record_minimal.ps1) と [run_minimal_matched.py](measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19/checks/run_minimal_matched.py)。
**これらは当日の再現資料で、その場所から直接実行する汎用ツールではない。** パス、revision、旧試行の移動・停止処理、待ち合わせファイルが固定されている。
今回の作業領域へ必要部分をコピーし、ROOT/WORK/TOOLKIT/PROFILE、基準版、入力、ジョブ一覧を更新する。古い試行の停止・移動・削除処理は引き継がない。
同じchecks内の `record.ps1`、`record_buffered.ps1` 等は不採用試行の資料であり、採用recorderの代わりに選ばない。

### ディレクトリと合図の順序

1. 今回専用のジョブ親ディレクトリを用意する。`wpr-ready/go/finished/saved/error.txt` が存在しない新しい実行先を選ぶ。
2. **先にCLI runnerを起動し、runnerが実行先を作成したことを確認してから `job.json` を公開する。** `run` は `mkdir(exist_ok=False)` を使うので、recorderが実行先を先に作ると失敗する。
3. ゲームが停止中の準備を終えると `wpr-ready.txt` にPID/TIDを書いて待つ。recorderはこの新しいreadyを待つ。
4. WPR開始成功 → 1秒待機 → `<mode>-begin -flush` 成功 → `wpr-go.txt` 作成。この順序で進める。
5. ゲームは指定の2,048 tickで停止し、`wpr-finished.txt` を書いて待つ。
6. `<mode>-end -flush` → `wpr -stop cpu.etl` → 欠落・マーカー検査と品質JSON保存 → `wpr-saved.txt` を作成してゲームを解放する。
7. runner正常終了と後述の採用条件を確認する。品質不合格の記録はゲームを解放できても採用しない。区間監査まで終えてから次の本へ進む。

古いready/savedを見て新recorderを走らせると、前のstopと次のstartが競合する。
recorderは二重起動せず、終了・失敗を確認してから切り替える。待ち時間には上限を設け、native gateの待ち上限（現行15分）とも整合させる。

### 実マップrunner

以下はHyperpathの通常条件の引数。`BUILD` は保存済みexe/PDB・langを持つルート、`FROZEN_INPUT` は固定パッケージ、`FRESH_RUN_DIR` はまだ存在しない実行先。
recorderと並行して起動する。基準と新版があるときは基準Legacy → 基準Hyperpath → 新版Legacy → 新版Hyperpathの順を記録する。

```text
python -B utils/passenger_precomputed_real_map.py run BUILD/Release/openttd.exe FROZEN_INPUT FRESH_RUN_DIR --ticks 2048 --timeout 2400 --measurement-wait 2400 --resume --precomputed --hyperpath --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1 --lang BUILD/lang --wpr-profile --wait-saved-replay
```

Legacyは `--hyperpath` を `--no-hyperpath` へ変更する。各本とも同じ固定入力から新しいプロセスで始める。
通常条件では `--cold-precomputed` / `--rebuild-precomputed` / `--full-rebuild-precomputed` や追加のwarm-up tickを入れない。
再構築を調べる場合は別条件として採り、通常条件の比較へ混ぜない。

**ロード完了待ちはバイナリの対応を確認する。** 0b8a21c0では `game_start` 時点に保存データの復元が残るため、停止中に `--wait-saved-replay` で復元完了を待ってからモード変更・warm-upへ進む。
旧97cdc0adの保存済みバイナリにはこのコマンドがないので、その版だけフラグを外した。旧版では従来のロード処理と下記の準備成功を確認できている。
新しいrunnerがオプションを受理しても、古い計測用バイナリにコマンドがあるとは限らない。互換性差はレポートへ記載する。

各本の計測開始前に、コンソールと `process.json` で次を確認する。

- 対応版では `Saved passenger replay: ready=true`。
- `Precomputed warm: ready=true`、背景処理の `completed=true`。
- 背景待ちの `tick_before == tick_after == 固定入力の開始tick`。準備中にシミュレーションを進めていない。
- `requires completed passenger save replay` 等の準備拒否がない。2,048 tick動いたという理由だけで採用しない。

各採取前にrunnerの `PROFILE.wait_for_idle` 等でホスト負荷を待ち、MSBuild nodemodeの残留workerも確認する。
計測中に別のゲーム、ビルド、テスト、WPA exportを走らせない。ユーザーのゲームを無断終了しない。
必要な調整は早めに行い、他チャットへの連絡はユーザーが許可した範囲で行う。

## 5. 1本ごとの採用条件

全項目がそろうまで、次の採取や結果の解釈へ進まない。

| 検査 | 採用条件 |
| --- | --- |
| 実行 | runner/game終了コード0、`simulation_timing_valid=true` |
| tick | 開始・終了が固定値、差が2,048、`actual-ticks.json` のsamplesも2,048 |
| 準備 | replay対応確認、warm成功、背景処理完了、準備前後のtick不変 |
| 同一性 | 入力・cfg・全アセット、exe/PDB、採取・集計profileのハッシュが予定と一致 |
| ETW | lost events=0、lost buffers=0、begin/end両方あり、begin < end |
| 区間 | readyの自PID/TIDを使用。kernelのprocess lifetimeに競合ビルド・テスト・ゲームが重ならない |
| 正しさ | canonical 24項目を同じモードの基準と比較。差があれば内容を保存 |

区間監査には次を使う。`MODE` は `legacy` または `hyperpath`。

```text
python -B utils/passenger_wpr_window.py FRESH_RUN_DIR TOOLKIT --mode MODE
```

process lifetimeのMIN/MAXも含めて監査する。プロセス一覧を採取前後に読むだけでは区間内の競合を見逃す。
欠落や開始マーカー欠如がある場合、全プロセス時間へ置換したり、残った区間に窓をずらしたりしない。
`xperf -tle` で欠落のあるETLを診断できても、性能比較の採用条件は満たさない。

canonicalはJSONをPython整数として読み、キー集合・24項目・値を完全一致で比べる。64bit digestを浮動小数点へ変換しない。
スイープ時期等が変わり同一を期待しない依頼では、その旨と時間・件数の比較条件を先に記録する。意図しない不一致を隠すために基準を更新しない。

## 6. validate / alternateは別の実行にする

WPRの性能測定には通常の `index` モードを使う。validate / alternateは同じ固定入力から新しい実行先へ起動し、性能測定の時間へ混ぜない。

validateだけを依頼され、時間を性能比較に使わない場合は `--validation-only` を指定できる。WPRとそのpilotは不要で、他プロセスを記録しつつ正確なtick数・準備完了・診断値・canonicalを確認する。
この指定では `simulation_timing_valid=false` が意図した値になるため、上の性能測定用採用条件とは区別する。既存のゲームやPGO処理を止めるための確認待ちを増やさず、性能の比較・要約には流用しない。

- validate: 通常runnerから `--wpr-profile` を外し、`--hyperpath-filter-mode validate` にする。ログのlookup行とmemo行を別々に保存し、該当するvalidatedが正でviolations=0であることを確認する。
- alternate: `--hyperpath-filter-mode alternate` または対応版の `--hyperpath-filter-alternate` を使う。ほかのfilter指定とは併用しない。
  現行の比較は索引対マスク。`indexed_us / indexed_walks` と `filtered_us / filtered_walks` を比べる。旧版のマスク対全走査と混同しない。

0b8a21c0では終了canonicalは一致した一方、**memo validated=13,769,660 / violations=13,655で検証不合格**だった。lookupのviolations=0やnativeテスト成功でmemoの失敗を打ち消さない。
0b8a21c0のmemoカウンターはuniqueグループ重複と再計算値不一致の2か所で増えるため、集計だけから全件の原因を断定しない。以後の版では検証対象やカウンターが変わり得るので、対象版の実装とログ項目を確認する。
検証不合格は結果として報告・保存し、同じ条件の再実行を繰り返して合格を待たない。製品修正は依頼範囲を確認する。

## 7. 採取完了後にexport・集計する

採取がすべて終わってから後処理を始める。`TOOLKIT` はxperf/WpaExporterを持つWindows Performance Toolkitの実在するディレクトリ。
ツールのバージョンを記録し、`BUILD/cpu-sampled.wpaProfile` も比較対象と同じものを置く。
Windows symbol cacheは既存のものを再利用できるが、製品PDBは必ずそのexeに対応させる。

```text
python -B utils/passenger_wpr_export.py BUILD TOOLKIT --output-name wpa --local-symbols-only
python -B utils/passenger_wpr_summarize.py BUILD EVIDENCE --export-name wpa
python -B utils/passenger_wpr_control.py CONTROL_EVIDENCE EVIDENCE
```

`BUILD/legacy` と `BUILD/hyperpath` が揃った構造を使う。exportのみ1本を指定する場合は `--mode legacy` / `--mode hyperpath`。
**summarizeの前にEVIDENCE自体を作成する。** 比較基準を `EVIDENCE/control-REV` に出すときは、その親も明示的に作成する。
exportは新しい出力フォルダーを要求する。既に成功したexportはハッシュと終了コードを確認して再利用し、失敗分だけ別の出力名でやり直す。
WPAのregistryアクセスがsandboxに拒否された場合は適切な実行権限でexportをやり直し、実マップの採取はやり直さない。

export後の確認:

- `export-command.json` の終了コード0、ETL/PDB/CSVのハッシュ、必要なCSV列、製品関数のシンボル解決。
- 圧縮CSVを展開したバイト列のハッシュが生CSVと一致。
- スタックとexclusiveの時間・サンプル数の保存則、支払い整合性。警告は内容・時刻を確認し、測定区間との関係を残す。
- 報告範囲はメインスレッドで `StateGameLoop` を含む `simulation_stack`。ロード・保存・停止中warm-upをtickのCPU時間へ混ぜない。
- inclusiveは1スタック内の同じ関数を1回だけ数える。親と子のinclusiveを足さない。ms/tickは対象CPU時間を実tick数2,048で割る。

loading walkはCargoLoad用の `StationCargoList::VisitBoardingCandidates<…>` 本体を選び、内側lambdaのinclusiveを加算しない。
`StepHyperpath` 配下のvector再確保は、その呼出し経路にある対象フレームの和集合で集計する。
PDBで表示される型名にはICFの別名があり得るため、表示名だけから処理内容を断定しない。サンプル0は時間0の証明ではない。

`passenger_wpr_control.py` のuntouched新/旧比は機械状態の変動を仮定した補正で、CPU周波数の実測ではない。
生値、サンプル数、補正係数、補正後、tick実時間を併記する。各組合せ1回の小差を確定的な高速化と断言しない。
ETW設定を変更した場合は同設定で採った基準を主比較にし、以前の採取設定との比較を別表にする。

## 8. 失敗時は必要な段階だけ戻す

| 症状 | 次に行うこと |
| --- | --- |
| prepareのアンカー不一致 | 長い採取前に生成位置を確認。計測スクリプトのコピーだけ修正し、configure/prepare/buildをやり直す |
| replay未完了、warm/background拒否 | 対応する停止中replay待ちを入れ、準備からやり直す |
| 開始マーカー欠落 | File方式・fresh合図・開始順を直し、短いpilotから確認 |
| events/buffers欠落 | 検証済みkernel専用profileの一致を確認。user collectorが残っていないか調べる。pilotで解決後に対象本だけ再採取 |
| 古い合図、WPR start/stop競合 | 自分のrecorderの終了を確認。新しいrun/job出力先で再開 |
| 区間内に他ビルド等が重なる | 負荷を調整し、その本だけ再採取。採取済みで無関係な本は保持 |
| symbol不足、export失敗、集計先の親がない | ETL/exe/PDBを保持し、symbol/実行権限/出力先を直して後処理だけ再開 |
| canonical不一致、validate非0 | 差分とログを保存し、不合格を報告。測り直しで原因を隠さない |
| 空き容量不足 | 不採用試行の小さい証跡を残し、不要な巨大生ファイルを整理。入力・採用ETL・対応exe/PDBを先に保管 |

空き容量は開始前と各本の後に確認する。失敗した標準CPU採取は1本でも大きくなり、過去には不採用分約29.6GBがたまった。
削除前に絶対パスを解決して今回の対象ディレクトリ内であること、稼働中プロセスが使っていないことを確認する。
不採用のログ・metadata・品質理由を保持し、元セーブ、Git追跡済み証跡、採用ETL、必要な旧exe/PDBを巻き込まない。

## 9. 保管・報告・push

採用した生ETL、固定入力、対応exe/PDBを作業ツリー外へ保管し、絶対パス・容量・SHA-256のmanifestをGitに残す。
ハードリンクのバックアップは同じファイル実体なので独立した複製とは数えず、以後書き換えない。
buildディレクトリを消しても復元・再集計できる状態にする。重複する巨大ZIPやsymbol cacheを無目的に増やさない。

Gitにはレポート、関数別inclusive/exclusive CSV、圧縮生CSV、tick・canonical・validate・品質・区間監査JSON、入力manifest、ビルド/採取/exportレシピとハッシュを含める。
当日の補助スクリプトも再現資料として保存し、採用したスクリプトと不採用資料を明記する。`.etl` / `.sav` / `.exe` / `.pdb` はコミットしない。
stage後にblobのハッシュとmanifestの対応、および `git diff --cached --check` を確認する。
WindowsのPythonでハッシュ対象のレポートを生成する場合は `write_text(..., newline='\n')` と対応する `eol=lf` 属性等で改行を固定する。作業ファイルだけでなくstage済みblobを照合し、検査が非0で終わったらcommit・pushへ進まない。
報告は結果、同一性・検証の成否、時間の生値と比較条件、ばらつき、失敗の理由を明記する。
この比較系列のpush先は `sjcl` の `merge/hyperpath-with-table-build`。現在のremote/branchを確認し、force pushせず、push後にremoteのcommit IDを照合する。

今後この手順を変更して成功した場合は、採用profile・版との互換性・失敗からの復旧方法を本書へ反映する。
