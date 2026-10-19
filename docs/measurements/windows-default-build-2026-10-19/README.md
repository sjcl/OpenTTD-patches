# 既定 Windows ビルドの証跡

条件と限界は [検証レポート](../../windows-default-build-2026-10-19.md) を参照。

- `build-identity.json`: ビルド時の base commit、実際の revision、変更ソースのハッシュ、全784テストの成功。
- `verification.json`: 実行ファイル・PDB・プロファイルのハッシュ、別配置先の実ソースから読み取った PGO 実行回数。
- `profile-identity.json`、`training-ticks.json`: 共通プロファイルの由来、4,096 world tick、データと入力のハッシュ。
- `compile-commands.json`、`cache-settings.txt`: 最終 Release の実コンパイル指定と選択設定。
- `build-and-ctest.log.gz`: 最終ビルドと784/784成功のログ。PGO の hash mismatch はない。
- `arena-exit-regression.log.gz`: ブロック再利用と終了時キャッシュ寿命の個別検証。
- `previous-shutdown-deadlock-stacks.log.gz`: 修正前に非侵襲接続で取得した、CRT 終了ロックと worker join の相互待ちのスタック。
- `portable/`: 別のディレクトリ（空白を含む）でも無名名前空間関数と間接呼び出しのカウンターを読み込む実ビルド fixture。
- `namespace-migration.log`: 旧データ内の9種類の名前を共通パスの hash へ変換した記録。
- `tools/`: 当該実機の検証・一度限りの変換に使ったスクリプト。再現時は repository 直下の ignored `build-windows-checks/` に置く。変換スクリプトだけは同フォルダーの `python/` に xxhash を必要とする。通常ビルド・CI・今後の標準学習手順には xxhash を追加しない。

元セーブ、素材、実行ファイル、PDB、raw profile、メモリダンプはコミットしていない。
ビルドと手動学習の正式な入口は `os/windows/Build.ps1` と `os/windows/PreparePGO.py`。
