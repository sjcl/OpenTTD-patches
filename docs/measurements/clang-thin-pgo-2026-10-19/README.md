# Clang ThinLTO + PGO の証跡

条件・数値・限界は [比較レポート](../../clang-thin-pgo-2026-10-19.md) を参照。

- `build-identity.json`: ソース・入力・各 executable / PDB・プロファイルのハッシュ、revision、CMake cache 設定。
- `builds/`: 生成 MSBuild project、CMake configure / build コマンド、コンパイル tlog、revision TU。
- `profiles/`: 元セーブで4,096 tick学習した IR プロファイルの識別情報、圧縮 indexed profile、集計と選択関数の正のカウンター。raw profile は build 内に保持する。
- `profile-use-checks.json`: 代表 LLVM bitcode object のハッシュとメタデータ。実行回数情報を読み込んだことを PGO 無効版との対照で確認。
- `option-checks.json`: 既定で PGO 無効、不正モード・非 Clang・欠損ファイル・ディレクトリ指定の拒否。
- `profile-invalidation.json` と `checks/invalidation/`: 有効な profile を同じパスで更新したときの CMake 自動再構成、スナップショット更新、object 再コンパイル、実行成功。
- `checks/tests.log.gz`: PGO 版の775テスト・10,745,430 assertionsの成功ログ。
- `summary.json` と `runs/wpr-*/`: 同じ1,024 tickの WPR sampled CPU、関数ごとの inclusive CPU、終了状態の一致。両トレースとも lost buffers / events は0。経路探索・乗車判定の対象関数一覧は summary に記録し、和集合は重複を一度だけ数える。PGO のインライン化によるスタック帰属の変化にも留意する。
- `summary-plain.json` と `runs/plain-*/`: WPR を使用しない参考測定。world は粒度の粗い `GetThreadTimes`、表構築 CPU は全プロセスの user + kernel 合計。
- `admin-launch.json` と `recorder.log`: 初期の取り消し・承認レビュー障害と、その後に成功した管理者ヘルパー起動・収録の記録。ETL 本体は build 内に保持し、識別ハッシュとエクスポート条件を保存した。
- `mp-baseline-pgo/` と `mp-pgo-baseline/`: 同じ元セーブで各4 worker、ネイティブネットワーク経由の両方向同期検証。
- `copied-tus/`: baseline / generate / pgo が共用した計測 TU。製品のアルゴリズム変更・強制 noinline はない。
- `tools/`: 当該実機の再現用ツール。ROOT・LLVM・SDK・依存ライブラリ・前回の計測ディレクトリの絶対パスと、元セーブ・素材を必要とする。再計測時は既存の結果を上書きせず別名を使い、build 内の `recorder-stop.txt` を取り除いて管理者ヘルパーを起動する。収録後は停止マーカーを戻す。

圧縮 `.profdata` は LLVM 19.1.5 用で、学習と同じソース・設定・TU パスで使用する。別構成のプロファイルとして扱わず、ソース・コンパイラ等を変えた場合は再学習する。
実行ファイル・PDB・raw profile・素材・セーブをコミットしていない。
