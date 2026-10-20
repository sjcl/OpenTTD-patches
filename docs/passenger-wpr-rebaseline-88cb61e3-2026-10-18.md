# 88cb61e3: 新しい定常セーブでのWPRと表の再構築比較

新しい固定入力を8462e6cdで各モード8,192 tick走らせ、Legacy・Hyperpathの定常セーブを凍結した。
その同じモードの入力から8462e6cdと88cb61e3を各2,048 tick実行した。
通常条件4本に加え、構築Workを含む再構築条件4本を記録した。
**両条件・両モードの新旧比較すべてで、終了canonical 24項目が完全一致した。**
64 bit digestを含め、Pythonの整数として比較している。

**今回の置換構築では高速化を確認できなかった。** 補正後のinclusiveは
Hyperpath `StepHyperpath` **+8.8%**、`TickPrecomputed` **+7.6%**、Legacy `TickPrecomputed` **+11.8%**。
Legacyの構築が遅くなっていないという結論も、この測定からは出せない。
tick全体の補正後変化はHyperpath -0.52%、Legacy -0.07%だった。
各条件1回の測定であり、一般的な性能差の確定には反復測定が必要である。

以前のセーブは回収できず、ユーザーの指示で別セーブに変更した。
[以前のHyperpath inclusive **2.954580420 / 3.010732959 ms/tick**](passenger-routing-steady-pair-8462e6cd-2026-10-18.md)は入力が異なるため、今回との直接比較には使えない。
以下の変化率は、今回の共通入力・同じ再構築条件で測り直した8462e6cdを基準にしている。

## 構築を含む区間のinclusive

通常の定常区間では構築Workが0だったため、`StepHyperpath`のサンプルを得られなかった。
そこで同じ凍結入力を使い、停止中に旧コスト20の表を準備してから転送ペナルティを17へ変更し、
**通常tickでの置換構築**を2,048 tick測った（`--rebuild-precomputed`）。
新しい表を停止中に完成させる`--full-rebuild-precomputed`は使っていない。
構築途中の2,048 tickを対象とし、公開行数とretained行数もcanonical照合に含めた。

| 対象（CPU ms/tick） | 8462e6cd | 88cb61e3 生値 | 生の変化 | 88cb61e3 補正後 | 補正後の変化 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Hyperpath `StepHyperpath` | 1.284596 | 1.494746 | +16.36% | 1.397452 | +8.79% |
| Hyperpath `TickPrecomputed` | 1.383265 | 1.591930 | +15.08% | 1.488310 | +7.59% |
| Legacy `TickPrecomputed` | 0.486960 | 0.522992 | +7.40% | 0.544404 | +11.80% |

Hyperpathの個別exclusive（simulation_stack全体、CPU ms/tick）:

| 関数 | 8462e6cd | 88cb61e3 生値 | 88cb61e3 補正後 | 補正後の差 |
| --- | ---: | ---: | ---: | ---: |
| `SetSearchLabel` | 0.148910 | 0.020521 | 0.019185 | -0.129725 |
| `StepHyperpath` 自身 | 0.613756 | 0.761343 | 0.711786 | +0.098031 |
| 目的地キュー `Radix::Insert` | 0.006833 | 0.066410 | 0.062088 | +0.055254 |
| 目的地キュー `Radix::Top` | 0.146455 | 0.100113 | 0.093597 | -0.052859 |

`SetSearchLabel`の時間は減っているが、`StepHyperpath`全体の減少にはつながっていない。
個別関数の帰属はインライン展開にも影響されるため、この内訳だけで原因を確定していない。
製品コードへの追加変更は行っていない。

CPU Usage (Sampled)のうち、`StateGameLoop`をスタックに含むサンプルが対象。
同じスタック中の各関数は1回だけ数えた。親子のinclusiveを足し合わせない。

補正には指定の `utils/passenger_wpr_control.py` を既定分類のまま使用した。
untouched時間の比 `新 / 旧` を共通の機械速度変動と仮定し、新版の時間をその比で割った。
これはサンプルからの推定で、CPU周波数の直接測定ではない。

| 条件 | Legacy補正係数 | Hyperpath補正係数 |
| --- | ---: | ---: |
| 通常 | 1.029183311 | 0.953685628 |
| 再構築 | 0.960668413 | 1.069622767 |

各条件1回のサンプリング測定であり、小さい増減は測定ばらつきを含む。
[再構築の関数別inclusive/exclusive差分CSV](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/function-before-after.csv)、
[指定スクリプトの出力](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/control.json)、
[対象関数の数値](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/focus.json)。

補正の再計算（リポジトリ直下で実行）:

```powershell
python -B utils/passenger_wpr_control.py docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/baseline docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/candidate
```

## 通常の定常区間

通常条件では新旧とも`StepHyperpath`が0サンプル、構築Workは0。
以下の`TickPrecomputed`は構築を行わない保守処理であり、構築性能の判断には使わない。

| 対象（CPU ms/tick） | 8462e6cd | 88cb61e3 生値 | 生の変化 | 88cb61e3 補正後 | 補正後の変化 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Hyperpath `StepHyperpath` | 0（0 samples） | 0（0 samples） | — | — | — |
| Hyperpath `TickPrecomputed` | 0.074237 | 0.063486 | -14.48% | 0.066569 | -10.33% |
| Legacy `TickPrecomputed` | 0.068374 | 0.079114 | +15.71% | 0.076870 | +12.43% |

[通常条件の関数別差分CSV](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/function-before-after.csv)、
[補正出力](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/control.json)。

Workの実行確認として、入力と終了時のcanonical `table_operations`も保存した。
これはCPU時間とは別の、表構築で処理した操作数である。
通常条件は入力との差。再構築では新しい表のカウンターが0から始まるため、終了値が計測区間のWorkになる。

| 条件 / 版 / モード | 入力のoperations | 終了のoperations | 計測区間のWork |
| --- | ---: | ---: | ---: |
| 通常 / baseline / legacy | 460,096,785 | 460,096,785 | 0 |
| 通常 / baseline / hyperpath | 189,560,629 | 189,560,629 | 0 |
| 通常 / candidate / legacy | 460,096,785 | 460,096,785 | 0 |
| 通常 / candidate / hyperpath | 189,560,629 | 189,560,629 | 0 |
| 再構築 / baseline / legacy | 460,096,785 | 134,152,192 | 134,152,192 |
| 再構築 / baseline / hyperpath | 189,560,629 | 134,152,192 | 134,152,192 |
| 再構築 / candidate / legacy | 460,096,785 | 134,152,192 | 134,152,192 |
| 再構築 / candidate / hyperpath | 189,560,629 | 134,152,192 | 134,152,192 |

## tick全体

再構築条件の`StateGameLoop` inclusive:

| 対象（CPU ms/tick） | 8462e6cd | 88cb61e3 生値 | 生の変化 | 88cb61e3 補正後 | 補正後の変化 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Legacy `StateGameLoop` | 37.097658 | 35.613862 | -4.00% | 37.071961 | -0.07% |
| Hyperpath `StateGameLoop` | 22.725088 | 24.179696 | +6.40% | 22.605816 | -0.52% |

通常条件の`StateGameLoop` inclusive:

| 対象（CPU ms/tick） | 8462e6cd | 88cb61e3 生値 | 生の変化 | 88cb61e3 補正後 | 補正後の変化 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Legacy `StateGameLoop` | 27.492690 | 27.925861 | +1.58% | 27.134001 | -1.30% |
| Hyperpath `StateGameLoop` | 14.367827 | 13.598936 | -5.35% | 14.259349 | -0.76% |

ドライバによる実測wall時間は次のとおり。ロード・停止中の準備・保存・ETL書き出しを含まない。

| 条件 / モード（wall ms/tick） | 8462e6cd | 88cb61e3 |
| --- | ---: | ---: |
| 通常 / legacy | 28.397909 | 28.840061 |
| 通常 / hyperpath | 15.594980 | 15.429938 |
| 再構築 / legacy | 39.145684 | 37.432176 |
| 再構築 / hyperpath | 24.889221 | 25.123544 |

## 入力と正しさ

元入力は手元の `SAVE2.sav`、SHA-256
`79fa148791f6abbf76f9b64852b475d2e937383d949413c4ffe55057f76c51c3`。
先に試した `ga58d47238.sav` は `Invalid passenger OD graph revision` でロードできず採用しなかった。
SAVE2はロード・再保存を確認し、前回の設定と246資産ファイルを復元して固定した。

定常化は両モードとも `13639680 → 13647872` の8,192 tick。
定常化と再ロードは状態確認専用とし、時間を性能比較に使っていない。
別ビルドが重なった準備実行の無効フラグも元のmetadataに保持した。

| モード | 凍結セーブSHA-256 | 再ロード | 通常の新旧canonical | 再構築の新旧canonical |
| --- | --- | --- | --- | --- |
| Legacy | `02e4ea48dbed6116a5db569509e0c3fcda98c21090f8e22382c527caec22b671` | 24/24一致 | 24/24一致 | 24/24一致 |
| Hyperpath | `3e8cfab5e70b3843c56b989891ab327bc75daa72ee7f5b232250d86eb6fade7e` | 24/24一致 | 24/24一致 | 24/24一致 |

有効なWPR 8本はすべて `13647872 → 13649920`、実際に2,048 tick進んだことを確認した。
同一条件・同一モードの新旧を比較する。通常と再構築、LegacyとHyperpathの間でcanonical一致は要求していない。
[通常の照合](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/canonical-comparison.json)、
[再構築の照合](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/canonical-comparison.json)、
[凍結・再ロード検証](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/seed-validation.json)。

## 記録と書き出し

両条件とも旧Legacy → 新Legacy → 旧Hyperpath → 新Hyperpathの順に記録した。
主スレッドaffinity mask 1、`--resume --precomputed --warm-precomputed --wait-background-ready`、
`--hyperpath-filter-mode index`。Legacyは`--no-hyperpath`、Hyperpathは`--hyperpath`。
準備中にStateTicksが進まないことを確認してから計測した。

WPRは `wpr -start CPU` → 開始マーカー → 2,048 tick → 終了マーカー → `wpr -stop`。
ゲームは通常権限、記録ヘルパーだけ管理者権限で実行した。
前回UACが取り消された追加実行は、開始ゲートがタイムアウトした失敗出力として保管し、有効8本から除外した。
再試行中に別ビルドが重なったLegacy 1本も、kernel lifetime監査で無効と判定して別保管し、取り直した。
[UAC取り消しの証跡](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/rejected-uac-cancelled-legacy/exclusion.json)、
[ビルド重複の監査結果](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/rejected-build-overlap-legacy/window-audit.json)。
終了済みビルドの孤立MSBuild待機ノードは、親の終了・子コンパイラなし・10秒間CPU使用なしを確認して終了した。
各実行前に60秒の静穏区間を置き、ETL内のkernel process lifetimeと開始・終了マーカーを照合した。
**有効8本すべて、計測区間に競合ビルド・テスト・別ゲームの重複なし。**
最後の新版Hyperpathでは終了マーカー後にビルドの再開を連絡したため、プロセス全体の無効フラグは残っている。
simulation区間の有効フラグとkernel lifetime監査が一致することを確認し、保存後処理の重複はtick比較に含めていない。

WPAExporter 11.7.395.48728は前回と同じ版。
[Microsoft公式ADK](https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install)のWPTを作業領域へ展開した。
各実行に一致するPDBで、CPU Usage (Sampled)のStacksとExclusive functionsをCSVへ書き出した。

| 記録 | ETL bytes | Lost Events / Buffers | 未解決の製品関数 |
| --- | ---: | ---: | ---: |
| 通常 / baseline / legacy | 2,400,190,464 | 0 / 0 | 0 |
| 通常 / baseline / hyperpath | 1,364,197,376 | 0 / 0 | 0 |
| 通常 / candidate / legacy | 2,437,939,200 | 0 / 0 | 0 |
| 通常 / candidate / hyperpath | 1,343,225,856 | 0 / 0 | 0 |
| 再構築 / baseline / legacy | 4,817,158,144 | 0 / 0 | 0 |
| 再構築 / baseline / hyperpath | 2,694,840,320 | 0 / 0 | 0 |
| 再構築 / candidate / legacy | 4,105,175,040 | 0 / 0 | 0 |
| 再構築 / candidate / hyperpath | 2,499,805,184 | 0 / 0 | 0 |

全範囲（all / main / simulation_stack）でFunction表とStacks表の時間・サンプル総数の保存を確認した。
生CSVはgzipで保存し、展開後のSHAを元CSVと照合した。
WPAのInvalidEvent警告は全文を保存し、すべて計測マーカーの外側であることを確認した。
不明スタックの量や全検査は[通常validation](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/validation.json)と
[再構築validation](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/validation.json)にある。

| 条件 / 版 | 関数別inclusive/exclusive CSV | summary・生CSVへの入口 |
| --- | --- | --- |
| 通常 / 8462e6cd | [Legacy](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/baseline/legacy-functions.csv) / [Hyperpath](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/baseline/hyperpath-functions.csv) | [summary](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/baseline/summary.json) |
| 通常 / 88cb61e3 | [Legacy](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/candidate/legacy-functions.csv) / [Hyperpath](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/candidate/hyperpath-functions.csv) | [summary](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/candidate/summary.json) |
| 再構築 / 8462e6cd | [Legacy](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/baseline/legacy-functions.csv) / [Hyperpath](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/baseline/hyperpath-functions.csv) | [summary](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/baseline/summary.json) |
| 再構築 / 88cb61e3 | [Legacy](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/candidate/legacy-functions.csv) / [Hyperpath](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/candidate/hyperpath-functions.csv) | [summary](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/candidate/summary.json) |

関数別CSVには全スレッド・主スレッド・simulation_stackの各範囲を含む。
全スレッド・主スレッドのexclusiveはWPA Function列、simulation_stackのexclusiveはスタック末端の値である。

## ビルドと保管

両版ともMSVC 19.44 x64、Release `/O2 /Ob2 /DNDEBUG /Zi /Zo`、assert有効、LTO無効、static CRT、
`/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO`。
8462e6cdは独立ソースアーカイブからビルドし、srcの1,869ファイルをGitと照合した（Windows改行変換を除いて一致）。
728 tests / 9,930,179 assertionsが成功。
88cb61e3にはWindows SDKの`far`マクロ衝突を避けるメンバー名変更だけを加えた（`b7cdf719e8`）。
729 tests / 10,074,203 assertionsが成功。
通常と再構築は同じexe/PDBを使用した。
[ビルド識別情報とSHA](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/build-identities.json)。

入力・定常セーブ2本・設定・全資産はworktree外にも保存した。
バックアップZIPは257エントリ、519,158,266 bytes、SHA-256 `a33468e8b2774fe34d593cf05ff1e846d7382da190431647e48ba888a24de994`。
[input-backup.json](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/input-backup.json)とZIP内の`RESTORE.txt`に場所・復元方法がある。
生ETL 8本と一致するPDBもworktree外へ保管し、SHAを確認した。
[通常の保管先](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/recording-backup.json)、
[再構築の保管先](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild/recording-backup.json)。

再実行・書き出し・検証スクリプトは[checks](measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/checks)に保存した。
専用スクリプトはリポジトリ直下の`build-rebaseline-88cb61e3/`で使ったコピーであり、再実行時も同じ配置に戻す。
ローカル生成物は`build-rebaseline-88cb61e3/`にあり、セーブ・資産・exe・PDB・生ETL本体はGitには含めていない。
