# 6aeaff68: 実マップHyperpath validate

同じ固定済み `gc3833d807.sav` を使い、`6aeaff68fc016a83eeca44d4484c9fc8024cabef` でHyperpathのvalidateを**1本、2,048 tick**実行した（2026-10-19 JST）。

**検証合格: lookup violations=0、memo violations=0、lookup_duplicates=0。**
終了canonical 24項目は、前回0b8a21c0のvalidate・Hyperpath通常実行、および97cdc0adのHyperpathとすべて完全一致した。
JSONの整数として比較し、64bit digestを浮動小数点へ変換していない。

```text
Passenger hyperpath lookup: rebuilds=2714; indexed_groups=203870; validated_groups=1011997; violations=0
Passenger hyperpath memo: hits=0; validated=13801003; conservative=17094; violations=0; lookup_duplicates=0
```

| 検証項目 | 件数 |
| --- | ---: |
| lookup validated_groups | 1,011,997 |
| lookup violations | 0 |
| memo validated | 13,801,003 |
| memo conservative | 17,094 |
| memo violations | 0 |
| lookup_duplicates | 0 |

前回0b8a21c0ではmemo validated=13,769,660、violations=13,655だった。
6aeaff68では、保存済み回答と再計算が違うとき、失敗系統のlocal policyがunknownならexact回答で追加照合し、保存済み回答と同じならconservativeへ数える。今回のconservativeは17,094件。
uniqueグループの重複も独立したlookup_duplicatesで確認した。新版では確定したtrue回答も保存するため、validated件数も旧版と単純には対応しない。

条件と確認:

- 入力セーブSHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットを前回manifestと照合し、変換・再保存せずに使用。
- StateTicks 3,197,276,448 → 3,197,278,496。実tick差とtickサンプル数はいずれも2,048。
- `--resume --wait-saved-replay --precomputed --hyperpath --warm-precomputed --wait-background-ready --hyperpath-filter-mode validate --main-thread-affinity-mask 1`。
- saved replay完了、warm成功、背景処理完了。準備前後のStateTicksは固定入力の開始tickから変化なし。cold・再構築条件・追加のwarm-up tickなし。
- 指定commitへ一時的にcheckoutし、MSVC 19.44 x64 Release、assert ON、LTO OFF、静的CRT、前回と同じフラグと計測用コピーでビルド。製品コードの追加変更なし。
- 全778 nativeテスト・10,745,598 assertionsが通った。
- **正しさ専用の `--validation-only`、WPRなし。** 別作業のPGO処理を停止せず実行し、重複プロセスはprocess.jsonに保存した。`simulation_timing_valid=false` はこの指定による意図した値で、所要時間を性能比較には使っていない。
- セーブ既存のNewGRF警告とerror pause解除の記録はコンソール・stderrに保持。終了時支払い整合性もtrue。

[検証結果](measurements/passenger-hyperpath-validate-6aeaff68-2026-10-19/validate-result.json)、[コンソール全文](measurements/passenger-hyperpath-validate-6aeaff68-2026-10-19/validate/on-1-console.txt)、[終了canonical](measurements/passenger-hyperpath-validate-6aeaff68-2026-10-19/validate/save-integrity.json)、[実行metadata](measurements/passenger-hyperpath-validate-6aeaff68-2026-10-19/validate/process.json)、[ビルド同一性](measurements/passenger-hyperpath-validate-6aeaff68-2026-10-19/build-identity.json)。

exe/PDBと終了セーブは作業ツリー外に保存し、[バックアップmanifest](measurements/passenger-hyperpath-validate-6aeaff68-2026-10-19/recording-backup.json)へ場所とハッシュを記録した。
固定入力は既存の同一ZIPを再利用し、[入力バックアップmanifest](measurements/passenger-hyperpath-validate-6aeaff68-2026-10-19/input-backup.json)のSHA-256を確認した。
採用した補助スクリプトはchecks配下に保存している。当日のパスとrevisionを含む再現資料であり、その場所から直接実行する汎用runnerではない。
