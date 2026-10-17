# 88cb61e3: WindowsビルドとWPR再測定の準備

**WPRは未実施。前回の定常セーブ本体を回収できず、canonicalの照合と性能差は未検証である。**
前回の2.95 / 3.01 ms/tickからの変化や、Legacyの構築時間について、新しい測定結果はない。
[機械可読の実施状況](measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/capture-status.json)にも、
記録なし・計測tick数0・canonical比較nullを明記した。

## Windowsで必要だった修正

指定の `88cb61e3756cdd88021bc87b8dd779a4c93473d5` を取得し、前回と同じMSVC 19.44、
Release `/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert有効、LTO無効、static CRT、完全PDBでビルドした。
未修正の版は `passenger_queue.hpp` のメンバー `far` がWindows SDKの `#define far` と衝突し、
`network_client.cpp` のコンパイルでC2208/C2760等が発生した。

`b7cdf719e88350ca6b46a787a4fbd676de7f3352` でメンバーと参照5箇所を `far_heap` に変更した。
アルゴリズム、Work、比較順序、データレイアウトの変更はない。
[置換だけであることの検証](measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/checks/rename-verification.json)を保存した。
以後の計測対象は厳密には **88cb61e3 + このWindows用の名前変更** になる。

修正後はゲームとテスト実行ファイルのビルドが成功し、
**729 test cases / 10,074,203 assertionsがすべて成功**した。
新しいキューとbinary heapの順序比較テストも含む。
これは正しさの検査であり、テスト実行時間を性能比較には使っていない。

[初回の失敗ログ](measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/checks/build.log.gz)、
[再ビルドの成功ログ](measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/checks/build-retry.log.gz)、
[全テストログ](measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/checks/native-all.log)、
[exe/PDB・ソース・計測用コピーの識別情報](measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/build-identity.json)。

## 不足している入力

前回の証跡はGit内に残っているが、以下のignoredディレクトリにあったセーブ本体は存在しなかった。
他のworktree、通常のOpenTTD保存先、ごみ箱も調べたが回収できなかった。
削除・移動の原因は特定できていない。

| 入力 | 前回の保存先（リポジトリ相対、元入力のみ別worktree） | 必要なSHA-256 |
| --- | --- | --- |
| Hyperpath定常 | `build-passenger-steady-8462e6cd/frozen-8192/input.sav` | `275544f1fd7bb16920ebd325c3a916ee06f164fcbe53abf0c1bb6409e9802f9f` |
| Legacy定常 | `build-passenger-legacy-steady-8462e6cd/frozen-8192/input.sav` | `b276c5de400651f646c129ab394bc97a686f38aa345b3a4a799e07c8a06289ca` |
| 定常化前の元入力 | worktree `5a69` の `build-hyperpath-cpu/frozen/input.sav` | `306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629` |

元入力だけを回収できた場合は、8462e6cdで各モードを8,192 tick実行して再生成し、
前回の凍結時canonicalとの一致を確認してから測定する必要がある。
別のセーブで走らせても、前回の終了canonical 24項目との一致の検証にはならない。

設定と資産246ファイルは、現在のOpenTTDデータと今回生成したbaseset/langから、
**前回の一覧とすべてSHA一致で復元できた**。
ローカルの `build-wpr-88cb61e3/recovered-assets/` に保存した。セーブ本体は含まない。
[復元状況](measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/checks/asset-recovery.json)、
[探索結果](measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/checks/input-search.json)、
[通常保存先・worktree内の全セーブのSHA探索結果](measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/checks/all-save-hashes-search.json)。

## 回収後の実行手順

再測定の基準は [8462e6cdの独立した定常入力のペア](passenger-routing-steady-pair-8462e6cd-2026-10-18.md)。
共通Hyperpath入力をLegacyへ切り替えた古いランは基準に使わない。

[measure.py](measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/checks/measure.py) は、
開始前に実際の入力SHA、設定SHA、資産全件のSHAを前回と照合する。
各モード2,048 tick、`--resume --precomputed --warm-precomputed --wait-background-ready`、
`--hyperpath-filter-mode index`、主スレッドaffinity mask 1を使う。
開始・終了は `3181127773 → 3181129821`、終了canonicalはPythonの整数で24項目すべて比較し、
差があれば `canonical-comparison.json` に保存して停止する。
不足入力でWPRを起動する前に停止することも[確認済み](measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/checks/missing-input-guard.log)。

```powershell
$taskBuild = (Resolve-Path build-wpr-88cb61e3).Path
$taskChecks = 'docs/measurements/passenger-hyperpath-wpr-88cb61e3-2026-10-18/checks'
python -B "$taskChecks/measure.py" $taskBuild --legacy-package PATH_TO_LEGACY_PACKAGE --hyperpath-package PATH_TO_HYPERPATH_PACKAGE --check-inputs-only
```

入力確認が成功してから、従来の `utils/passenger_wpr_record.ps1 -Build $taskBuild` を
管理者PowerShellで開始し、ゲーム用の通常権限PowerShellで上のコマンドから
`--check-inputs-only` を外して実行する。各パッケージには前回の `package.json` も戻す。
Hyperpathのみを回収できた場合は `--mode hyperpath` とそのパッケージだけで実行できる。
その場合、記録側にも `-Mode hyperpath` を指定する。

WPAExporter/xperfは以前のignored配下のコピーも見当たらず、CSV書き出し前に再配置が必要。
記録後は `passenger_wpr_export.py` のマーカー・kernel process lifetime監査、
`passenger_wpr_summarize.py` のinclusive/exclusive集計を従来どおり使う。
新旧同モードのcanonical一致を確認してから関数差を計算する。

| 比較対象（inclusive、CPU ms/tick） | 前回8462e6cd | 今回 |
| --- | ---: | --- |
| Hyperpath `StepHyperpath` | 2.954580420 | 未計測 |
| Hyperpath `TickPrecomputed` | 3.010732959 | 未計測 |
| Legacy `TickPrecomputed` | 1.194898096 | 未計測 |

今回のビルド、PDB、復元資産はローカルのignored `build-wpr-88cb61e3/` にある。
Gitにはソース修正とビルド証跡・再実行スクリプトを含め、セーブ・資産・実行ファイルは含めていない。
