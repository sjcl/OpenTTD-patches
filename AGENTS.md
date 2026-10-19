# Repository instructions

## Passenger の実マップ・性能計測

Passenger / Legacy / Hyperpath の実マップ計測、WPR採取、過去の計測との比較を依頼されたら、**準備・ビルド・採取を始める前に [計測手順書](docs/passenger-wpr-runbook.md) を読むこと**。
引き継ぎ後や作業再開時にも、同書と対象の前回レポートから入力・基準版・採用条件を確認する。

- 採取設定は [kernel CPU専用プロファイル](utils/passenger_wpr_cpu.wprp) を起点にし、短いpilotの欠落0・両マーカーを確認してから実マップへ進む。
- 固定入力、対応するexe/PDB、準備完了、正確なtick数、WPR区間、競合プロセスを確認する。古い合図ファイルや未完了の保存データ復元を持ち込まない。
- 採取条件を変えたら、比較基準も同条件で採取する。後処理だけの失敗で実マップを取り直さない。
- canonical一致とvalidate成功は別々に判定する。violationsが非0なら、その検証を成功と報告しない。
- 入力・採用ETL・対応exe/PDBの保管先とハッシュを記録し、手順と小さな証跡をGitに残す。ignoredのbuildディレクトリだけを引き継ぎ先にしない。

手順変更で新たな失敗を解消した場合は、次回の再発を防ぐため同書も更新する。
