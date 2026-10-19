# Passenger停止中準備・保存状態復元の合成ベンチマーク

実マップやWPRを使わず、固定seedの小～中規模サービス網でスケジューラを比較する。
実マップのtick性能・ETLの採取条件やcanonical/validate検証の代わりにはしない。

## 実行

1. 基準commitを固定する。今回の基準は `79636e49744a0b0caa9f174f87b5490aa3daca9b`。
2. 通常のnativeテストとGUI版を、同じコンパイラ・最適化・assert設定でビルドする。
3. 必要なら `python -B utils/passenger_parallel_reference_prepare.py BUILD --reference COMMIT` で基準のテスト専用TUを生成し、出力された `reference.cmake` の絶対パスを `-DCMAKE_PROJECT_INCLUDE=...` に指定して再configure/buildする。ゲーム本体には参照実装をリンクしない。
4. `openttd_test.exe --reporter compact` を実行する。`[preparation]` はWork単位の公開状態比較、遅延・例外・timeout・取消し、混在世代の保存復元、GUIの実ゲームループを含む。参照TUがあれば準備中の公開状態も基準実装と比較する。
5. ビルドとテストが終了し、他のゲームや計測が停止してから、次を実行する。出力先は新規ディレクトリにする。

```text
python -B utils/passenger_preparation_benchmark.py BUILD/Release/openttd_test.exe EVIDENCE --side 16 --lines 100 --workers 0,1,4,8 --rounds 2 --pinned-reference
```

参照TUなしなら `--pinned-reference` を省略する。その場合の初期構築参照は同じバイナリの非ブロッキング `StepParallel` である。
最初は `--side 8 --lines 30 --workers 0,4 --rounds 1` のpilotで実行・出力・digest一致を確認できる。

各ケースを新規プロセスで実行し、2巡目はケース順序を反転する。個々の組合せは2回なので信頼区間ではない。
`OPENTTD_*` を継承せず、指定した固定seed・網サイズ・worker数・モードだけを設定する。
全ケース正常終了、モード内の最終公開digest・論理Work一致をrunnerが検査する。
Windowsでは各ケースの直前と実行中100msごとに他のビルド・テスト・ゲームを検査し、検出したら自分の計測だけを停止する。
合成DBのgenerationはシードしたserviceのgeneration以上にし、routing設定も有効にする。前者を欠くとLoadが正しく拒否し、後者を欠くとフェーズ診断がDisabledに分類される。

実ゲームループでサービス発見を含む初期構築全体を測る場合は、同じexeで次を使う。

```text
python -B utils/passenger_preparation_benchmark.py BUILD/Release/openttd_test.exe GUI_EVIDENCE --workers 4 --rounds 2 --gui-only
```

こちらは固定seedの32×32マス上に240本、各12停車の実fixture車両を置く。workerスケジューラは両方とも新版とし、旧27ms周期待機の有無だけを比較する。
実ウィンドウ描画はモックで、音声はnull。StateTicksは固定し、全公開digestとWorkを照合する。
`--side` / `--lines` はGUI fixtureの形状を変えない。

## 読み方

- `initial-reference`: 基準TUの通常の `StepParallel`。TUなしでは現行版の同API。
- `nonblocking-current`: 現行版の通常の `StepParallel`。参照TUを使う場合だけ実行し、通常経路の退行を比較する。
- `initial-preparation`: `StepPreparation`。固定したサービス入力からグラフと全目的地を構築する。車両の初期発見とGUI周期は含めない。
- `replay`: 旧世代を持ち、新世代を65,536 Workだけ公開したDBを保存し、その保存位置まで `StepLoad` で復元する。保存バイト列と両世代の全公開digestが元と一致することをnative側で検査する。workers=0は従来の直列探索を参照する。

`wall_ns` は各native窓の実時間。`graph_ns` / `retained_ns` / `tables_ns` は各スライス内の実時間で、結果待機を含む。
製品コンソールの `Passenger preparation phase wall` はGUI・通信に返したスライス間隔も含む。
`main_cpu_ns` / `worker_cpu_ns` はWindows/LinuxのスレッドCPU時計。短い試行はOS時計の粒度に注意する。
worker CPU / row実時間を同時稼働したコア相当数、その値 / 実worker数をworker利用率の目安として計算できる。
workerのsearch実時間をCPU時間と混同しない。復元ではworker CPUに現世代の先行探索も含まれ得る。

`peak_working_set` はプロセス全体のピークで、保存スナップショットを作る未計測のfixture準備も含む。
`workspace_bound` は既存の全世代共通512 MiB予算に対する保守的な予約量で、実メモリ使用量ではない。
要求worker数と実worker数を区別する。0は明示的な並列無効であり、fallbackを意味しない。
`serial_rows` はworker結果を使えず直列へ移った行数で、元々workers=0だった直列行を含めない。

## GUIとネットワーク

GUIテストはMockEnvironmentでBlitterを初期化し、null音声を使う。workerを遅延させて、
次のゲーム呼出しが通常tick時刻を待たず実行可能になること、描画スレッドがスライス間にロックを取れること、
StateTicksが不変であること、準備完了後に通常周期へ戻ることを実ゲームループで検査する。
worker起動前にフックを設定する。起動後に設定しても既存generationのフックは変わらない。

マルチプレイは実packet handlerの参加・準備バリアのnativeテストを併用する。
実際の複数PC間TCP通信の長時間試験と、実マップでの操作応答・メモリピークは別の検証範囲とする。

結果のJSON、raw log、ビルド条件、基準commit、参照生成物のhash、exe/PDBの保管先・hashをGit追跡する。
Gitへ保存するテキスト証跡はLFへ統一し、ログ末尾の空行だけを除く。記録行と数値は変更しない。
exe/PDB自体はGitへ追加しない。計測途中に失敗した場合はログを保持し、原因を直して新規出力先で実行する。
