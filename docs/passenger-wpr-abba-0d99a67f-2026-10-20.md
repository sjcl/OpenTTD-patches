# 6c11c67f / 0d99a67f 計測：canonical不一致でABBA停止

A=`6c11c67fd08115ce370fbfb182cdc952f37d75fd`、B=`0d99a67f800bbc13d8e2c0b8a48be07d3e25dd13`。2026-10-20 JST、同じ固定セーブからABBAを開始したが、**B1 Hyperpathでcanonical 24項目中11項目が不一致となり、手順書の基準でキューを停止した。ABBAは未完了で、性能改善の判定はできない。**

A1（Legacy→Hyperpath）とB1（Legacy→Hyperpath）の4本を保存した。A1の2本とB1 Legacyは、前回6c11c67fおよび今回A1の同じモードとcanonicalが一致した。B2とA2は未実行。不一致を消すための再実行・基準更新は行っていない。

## canonicalの差分

開始tick・終了tickと入力・exe/PDBの識別、復元と準備完了を確認した。B1 Hyperpathも2,048 tick、欠落0、競合負荷なしだが、結果の挙動が異なる。

| canonical項目 | A1 Hyperpath | B1 Hyperpath |
| --- | ---: | ---: |
| `cargo_units` | 546198 | 546082 |
| `company_deliveries` | 13595 | 13647 |
| `database_digest` | 16064563056932437984 | 5555598662768065810 |
| `journey_packets` | 456954 | 456992 |
| `known_od_units` | 467965 | 468008 |
| `loading_digest` | 5759760310152142397 | 13110572373424954604 |
| `ownership_digest` | 5786729106522441514 | 6636989440178595733 |
| `packet_state_digest` | 12763629749201381160 | 3411611219648325321 |
| `packets` | 469547 | 469581 |
| `payment_digest` | 13774651420232285578 | 2424413008291368806 |
| `published_revision` | 1777 | 1776 |

[完全な差分](measurements/passenger-wpr-abba-0d99a67f-2026-10-20/canonical-failure.json)、[4本の照合](measurements/passenger-wpr-abba-0d99a67f-2026-10-20/canonical-comparison.json)。digestはPython整数で比較した。変更したキャッシュが原因か、その中のどの経路かは、この終了状態の比較だけでは断定していない。

## validate 1本（B、Hyperpath）

同じ固定入力から新規プロセスで`--hyperpath-filter-mode validate --validation-only`を1本だけ実行した。exact tick、replay・warm・背景処理の完了、準備中tick不変を確認した。性能集計には含めない。

| 診断 | validated | violations | lookup_duplicates |
| --- | ---: | ---: | ---: |
| memo | 13,603,108 | 0 | 0 |
| lookup | 1,046,780 | 0 | — |

**診断カウンターの条件は合格した。** memoとlookupの検証件数は正で、violationsとlookup_duplicatesは0。
ただしvalidate終了canonicalもA1 Hyperpathと11項目が異なる。B1 indexのcanonicalとも異なる。[indexとの比較](measurements/passenger-wpr-abba-0d99a67f-2026-10-20/validate-canonical-comparison.json)。
`validate-result.json`の`passed=false`はcanonical条件を含む総合判定であり、違反カウンターが非0だったという意味ではない。
`simulation_timing_valid=false`はvalidation-onlyの意図した値。[判定・レシピ](measurements/passenger-wpr-abba-0d99a67f-2026-10-20/validate-result.json)、[診断ログ](measurements/passenger-wpr-abba-0d99a67f-2026-10-20/validate/on-1-console.txt)。

## 指定関数の診断用生値

**各版1本だけ、かつcanonicalが異なるため、以下を同じ挙動に対する速度改善と解釈しない。ABBA平均ではない。**
メインスレッドで`StateGameLoop`を含むSampledProfileのCPU ms/tick。`SharedHyperpathPlan self`はスタック末端だけ、ほかはinclusive。
同じスタック内の同じ関数は1回だけ数える。Resolve / Board unionはResolveHyperpath、BoardHyperpath、PassengerPrecomputedTable::Boardのいずれかを含むスタックの和集合で、親子を重複加算しない。
LTOで消えた関数の0サンプルは時間0の証明ではない。

| Hyperpath対象（CPU ms/tick） | A1 | B1 | samples A/B |
| --- | ---: | ---: | ---: |
| `MayUseHyperpathPolicy` | 0.520876 | 0.267059 | 1067 / 547 |
| `MayUseHyperpathPolicy self` | 0.130831 | 0.064923 | 268 / 133 |
| `ResolveHyperpath` | 0.213814 | 0.208458 | 438 / 427 |
| `BoardHyperpath` | 0.000000 | 0.155731 | 0 / 319 |
| `Board` | 0.172818 | 0.141081 | 354 / 289 |
| `ResolveHyperpath / Board union` | 0.213814 | 0.208458 | 438 / 427 |
| `SharedHyperpathPlan self` | 0.052728 | 0.002930 | 108 / 6 |
| `SharedHyperpathPlan` | 0.102512 | 0.078613 | 210 / 161 |
| `StateGameLoop` | 16.676257 | 16.669342 | 34144 / 34129 |

4本の各値:

| 順序 | 版 | モード | simulation CPU ms/tick | tick実時間 ms/tick | MayUse inclusive | Resolve inclusive | Board inclusive | Shared self |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 01-A1/legacy | A | legacy | 17.272245 | 17.353298 | 0.889012 | 0.186517 | 0.149901 | 0.000975 |
| 01-A1/hyperpath | A | hyperpath | 16.676257 | 16.885985 | 0.520876 | 0.213814 | 0.172818 | 0.052728 |
| 02-B1/legacy | B | legacy | 18.653306 | 18.764736 | 0.715758 | 0.197232 | 0.162575 | 0.003418 |
| 02-B1/hyperpath | B | hyperpath | 16.669342 | 16.827189 | 0.267059 | 0.208458 | 0.141081 | 0.002930 |

[指定関数CSV](measurements/passenger-wpr-abba-0d99a67f-2026-10-20/focus-runs.csv)、[全関数CSV](measurements/passenger-wpr-abba-0d99a67f-2026-10-20/function-before-after.csv)、[集計方法・未完了フラグ](measurements/passenger-wpr-abba-0d99a67f-2026-10-20/comparison.json)。CSVのmean/min/maxは各版1本の同じ値であり、反復による範囲や信頼区間を表さない。untouched補正は行っていない。

## 条件・品質・保管

- A/BともClangCL / LLD 23.1.3、ThinLTO、assert OFF、PGO OFF、静的CRT、`/O2 /Ob2 /MT /Z7`、`NDEBUG`。実コマンド、bitcode、compiler-rt builtinsとmanifestを検証した。Aは前回の独立保存済みexe/PDBをハッシュ照合して再利用。Bは対象commitのソースから同じレシピで作成。
- Bの全781 native tests、11,281,973 assertions成功。計測フックはコピーTUのみで製品ソースは変更していない。対応PDBの内部関数名を事前確認。
- 固定セーブ46,620,248 bytes、SHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットも前回manifestと一致。StateTicks **3,197,276,448 → 3,197,278,496**、各本2,048 tick。
- `--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。準備完了、準備中tick不変。追加warm-up tick・cold・再構築指定なし。
- kernel CPU専用File profile、1ms、1,024×1,024KiB。pilot成功後に採取。4本すべて欠落events/buffers 0、両マーカーあり、区間監査で競合ゲーム・ビルド・テストなし。
- WPA製品関数の未解決0、時間・サンプル保存則、支払い整合性、gzip CSVと生CSVのハッシュを検証。InvalidEvent警告はマーカー外。
- 生ETL4本・終了セーブ5本・A/B exe/PDBを作業ツリー外へ独立コピーして読み取り専用化。固定入力は既存ZIPをハッシュ再照合。[保管manifest](measurements/passenger-wpr-abba-0d99a67f-2026-10-20/recording-backup.json)、[入力ZIP](measurements/passenger-wpr-abba-0d99a67f-2026-10-20/input-backup.json)。

[採取品質とcanonical・validateの独立判定](measurements/passenger-wpr-abba-0d99a67f-2026-10-20/validation.json)。checksの採用スクリプトは`prepare.py`、`check_build.py`、`probe.py`、`measure.py`、`record.ps1`、`validate.py`、`post_partial.py`、`analyze_partial.py`、`package_partial.py`、`verify_partial.py`。`post.py` / `analyze.py` / `package.py` / `verify.py` / `after.py`は当初の8本完了用レシピで、途中停止後の集計・保管には使っていない。全て当日のパスを含む再現資料であり、その場所から実行する汎用ツールではない。
