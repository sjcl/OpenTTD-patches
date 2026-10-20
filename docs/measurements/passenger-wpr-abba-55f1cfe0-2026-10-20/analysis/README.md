# 補遺: Legacy の `RetiredPlan` が増えた件（2026-10-20）

[55f1cfe0 の ABBA](../../../passenger-wpr-abba-55f1cfe0-2026-10-20.md) を採取した後に追加で解析した。
レポート本体は `report-sha256.json` でハッシュを固定しているので書き換えず、この補遺に分けた。新しい採取はしていない。

## 結論

- Legacy の `RetiredPlan` の inclusive は 0.390861 → 0.460900 ms/tick（+0.070）。2 本ずつの範囲は重ならない。
- 増えた分は、ほぼすべて `MayUseHyperpathPolicy` から呼ばれたときの `RetiredPlan` の **self**（0.0474 → 0.1084）。
  - 各本の self: A1 0.0405、A2 0.0542、B1 0.1020、B2 0.1147。
  - 子の `PlanPrecomputed`（`retired_plans` に新しく挿入するときだけ呼ばれる）は 0.3106 → 0.3049 で変わっていない。挿入回数はほぼ同じと見られる。
  - `operator new` は +0.008、`RefreshBoardingRevision` は +0.003、`SharedHyperpathPlan`（Legacy では先頭で nullptr を返す）は +0.003。
- **機械語は同一だった。** 両版の `RetiredPlan` はどちらも 1,201 bytes・312 命令。違いは、`hyperpath_memberships` を削ったことで context のメンバーのオフセットが 0x10 ずれた 8 命令だけ（[差分](retired-plan-disassembly.diff)）。
- したがって、コード生成やインライン化の差ではない。考えられるのは、`retired_plans` を引く回数が増えたか、1 回あたりのコストが増えたか（メモリ配置の変化によるキャッシュミスなど）。どちらなのかは、サンプリングだけでは区別できない。
- Legacy の `MayUseHyperpathPolicy` は全体で −0.277 ms/tick 下がっているので、差し引きでは改善している。この経路は、モード切り替え直後に保存された Hyperpath 意図を計画し直すときに使われる。過渡状態に特有で、Hyperpath の通常 tick（`RetiredPlan` 0.073 → 0.053）には影響していない。
- 原因を確定するには、`retired_plans` の lookup と挿入の回数を数える診断カウンターが必要。

## 再現方法

- 呼び出し元・呼び出し先の分解: `python -I analysis/frame_neighbours.py . PassengerJourneyLoadingContext::RetiredPlan analysis/retired-plan-neighbours.json`（計測ディレクトリで実行する）。
  - 入力は各本の `CPU_Usage_(Sampled)_Stacks.csv.gz`。各行は 1 本の完全なスタックと、その葉の重み。`StateGameLoop` を含むスタックだけを数え、2,048 tick で割った。
  - 結果: [retired-plan-neighbours.json](retired-plan-neighbours.json)。
- 逆アセンブル: 保管先 [recording-backup.json](../recording-backup.json) の exe/PDB を使った。
  - A `openttd.exe` SHA-256 `bcf35a4918bc5f1e5c2f2bf7378953940bc5619354225e7906372dac4390483c`、PDB `e2dc60fc0d82a553ed56b367c51dda198dae7f73ff04082354b2b5db381eafe8`。
  - B `openttd.exe` SHA-256 `9ca81822edb47e137766d084a71567d9b3d697146bfd2853ca8d12c9823111b7`、PDB `5de032cbf5c90b9c029dd2906b627af501abe45b26c9d12ab80c7786e6dfc018`。
  - どちらも baseline/build-identity.json と一致した。
  - LLVM 23.1.3 の `llvm-pdbutil dump --symbols` で `S_LPROC32 PassengerJourneyLoadingContext::RetiredPlan` のアドレス（A `0001:5082864`、B `0001:5083536`、code size 1201）を取った。
  - `llvm-objdump -d --no-show-raw-insn` で `0x140001000 + offset` から 1,201 bytes を出力し、行頭アドレスと 6 桁以上の即値アドレスを正規化して diff した。
