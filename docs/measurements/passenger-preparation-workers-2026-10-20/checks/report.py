"""Generate the report's numeric tables from the adopted summary, without rerunning games."""
import json
from pathlib import Path

DATA = Path(__file__).resolve().parents[1]
SUMMARY = json.loads((DATA / "summary.json").read_text())
ROWS = {(r["scenario"], r["mode"], r["requested"]): r for r in SUMMARY}


def value(scenario, mode, workers, field="wall_ns"):
    return ROWS[scenario, mode, workers][field]["mean"]


def interval(scenario, mode, workers, field="wall_ns"):
    v = ROWS[scenario, mode, workers][field]
    return f"{v['mean'] / 1e6:.3f} [{v['min'] / 1e6:.3f}, {v['max'] / 1e6:.3f}]"


def main():
    text = """# Passenger初期構築・旧世代復元の並列化（2026-10-20）

`perf/passenger-preparation-workers`。基準は依頼時にfetchした最新の `sjcl/jgrpp-ya`、`79636e49744a0b0caa9f174f87b5490aa3daca9b`。
保存形式・ネットワークプロトコルの変更はない。通常tickのWork予算と非ブロッキングAPI、保存済みWork位置の意味を維持した。

## 変更

- 旧世代と現世代の保存位置復元を `StepPreparation` へ接続。既存の凍結グラフ・目的地worker・2×workerの先行キュー・全世代共通512 MiB予約上限を再利用する。
- 停止中だけ、目的地の結果をcondition variableで1呼出し最大1ms待つ。未完了ならWorkを消費せずGUI/通信へ返す。2秒の遅延、worker例外・起動失敗・予算不足では直列へ戻れる。結果は従来の目的地順・Work位置で公開する。準備中の単なる遅れでは16回missの抑制を作動させない。
- 停止中のmain処理は最大4,096 Workに分割。サービス取得のコルーチン再開単位は16～512、グラフ準備は128～4,096の範囲で実時間に応じて調整する。通常tickの論理Workには時間を使わない。
- GUIドライバは、直前のループが実際にローカル準備を進め、まだ準備が残る場合だけ次を直ちに実行可能にする。ゲーム状態ロックは各約8msの外側スライスで解放される。完了・相手待ちでは通常周期へ戻る。nullドライバと既存の送受信・参加barrierは変更しない。
- 復元済み/対象目的地の表示、フェーズ別実時間、main CPU、worker実行中/未完了数、hit・直列移行・wait・timeout・予約量をローカル診断へ追加。保存・同期状態へ含めない。

旧世代の不要行を復元前に省く最適化は見送った。行番号、到達可能索引、Hyperpathの候補/ポリシー、メモリenvelopeと保存Workに関係するため、現状の行skipだけで同一復元を証明できない。従来どおり全旧目的地を復元し、新目的地の公開と同じ位置で退役させる。

## 検証と計測条件

native **783ケース / 10,514,867 assertions成功**、実進捗ウィジェット1ケース / 100 assertions成功。
Legacy/Hyperpathの組合せ、worker 0/1/4/8、任意Workでの公開全フィールド、混在世代、保存バイト列、途中参加の既存packet handler/barrier、遅延・例外・timeout・取消し・メモリ拒否を検査した。
準備APIは基準commitから生成した独立クラスの旧実装ともWork単位で比較した。GUIの実ゲームループでtick不変、描画側のロック取得、待機削減と完了後の周期復帰も検査した。

ClangCL 23.1.3、x64 Release `/O2 /Ob2 /MT /Zi`、assert ON、LTO OFF、PGO OFF、32 logical CPU。
合成網は固定seed `0x0d16e57`、32×32マス、240本、実際に使う894駅。worker数は要求0/1/4/8すべて実数と一致した。
表構築は基準TUの `StepParallel`、新版の同API、新準備APIを比較。復元は旧世代と65,536 Workまでの新世代を持つ同じDBを保存し、`StepLoad` が保存位置まで復元する時間を測る。workers=0は従来の直列探索の参照で、旧exe全体の再実行ではない。
各組合せ2回、2巡目は全体の順序を反転。各本を新プロセスで実行し、競合を開始前と実行中100msごとに検査した。全64本正常終了、Workと公開digest一致。GUI8本も別に全件一致した。
以下は平均 [実測最小, 最大] のms。2回の範囲は信頼区間ではない。

## 旧世代復元（最優先）

| モード | worker | 旧世代復元 ms | 保存状態復元全体 ms | main CPU ms | peak MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
"""
    for mode in ("legacy", "hyperpath"):
        for workers in (0, 1, 4, 8):
            text += f"| {mode} | {workers} | {interval('replay', mode, workers, 'retained_ns')} | {interval('replay', mode, workers)} | {value('replay', mode, workers, 'main_cpu_ns') / 1e6:.3f} | {value('replay', mode, workers, 'peak_working_set') / 1048576:.1f} |\n"
    for mode in ("legacy", "hyperpath"):
        before = value("replay", mode, 0, "retained_ns")
        after = value("replay", mode, 8, "retained_ns")
        text += f"\n{mode}の旧世代復元はworker 8で **{before / 1e6:.3f}→{after / 1e6:.3f} ms（{(1 - after / before) * 100:.2f}%短縮）**。"
    text += """

全並列復元で894/894のworker hit、serial_rows=0、fallback=0、search_work=cache_work、cancelled_work=0。
worker 1は並列に主探索できる本数が増えず、Hyperpathでは直列より遅い。workerを増やせば必ず速いとは扱わない。
Legacyのworker 8でもグラフ準備2世代が約223ms残り、復元全体の約59%を占める。Hyperpathではmainの行仕上げ・ポリシー公開が残り、main CPUが実時間に近い。
復元のpeakはfixture作成も含むプロセス全体であり、復元窓だけの追加メモリではない。

## 初期テーブル構築とworker利用

固定サービスからグラフと全行を作る時間。GUI周期とサービス発見は次節で別に測る。

| モード | worker | 基準の非ブロッキング構築 ms | 停止中専用構築 ms | worker利用率概算 | peak MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
"""
    for mode in ("legacy", "hyperpath"):
        for workers in (0, 1, 4, 8):
            use = ROWS["initial-preparation", mode, workers].get("worker_utilization")
            pct = f"{use['mean'] * 100:.1f}%" if use else "—"
            text += f"| {mode} | {workers} | {interval('initial-reference', mode, workers)} | {interval('initial-preparation', mode, workers)} | {pct} | {value('initial-preparation', mode, workers, 'peak_working_set') / 1048576:.1f} |\n"
    text += """

利用率はOSのworker CPU合計 / 行構築の実時間 / 実worker数。Windows時計の量子化と計測窓後の小さいcleanupを含み、100%をわずかに超える値もある。全CPU32個に対する利用率ではない。
worker 4はほぼ全稼働。worker 8はLegacy約92%、Hyperpath約66%で、後者はmain側の行仕上げが先のボトルネックになる。
停止中専用の全並列ケースで894 hit / serial_rows=0。0/1の小差や8の改善頭打ちもそのまま残した。workspacesの既存設定・メモリ上限を越えてスレッドを増やしていない。

通常APIについても同じバイナリ内の基準TUと比較した。

| モード | worker | 基準StepParallel ms | 新StepParallel ms |
| --- | ---: | ---: | ---: |
"""
    for mode in ("legacy", "hyperpath"):
        for workers in (0, 1, 4, 8):
            text += f"| {mode} | {workers} | {interval('initial-reference', mode, workers)} | {interval('nonblocking-current', mode, workers)} |\n"
    text += """

この合成比較と公開順序の差分テストでは大きな退行は見られないが、2回の揺れを通常tick全体の改善/無劣化の証明とはしない。実マップのWPR/tick性能比較は今回実施していない。

## GUIでの初期構築全体

240台の実fixture車両を配置し、サービス発見→グラフ→全経路表完成まで、実 `VideoDriver::GameLoop` を呼ぶ。固定seed、32×32マス、各12停車、858駅。
両方とも新workerスケジューラを使い、比較側だけ旧27ms周期を再現した。これは周期待機削減だけの効果を分離する実験で、旧exe全体との比較ではない。描画と音声はモック/null。

| モード | 旧27ms周期 ms | 新連続スライス ms | 短縮 |
| --- | ---: | ---: | ---: |
"""
    for mode in ("legacy", "hyperpath"):
        before, after = value("gui-tick-cadence", mode, 4), value("gui-current", mode, 4)
        text += f"| {mode} | {interval('gui-tick-cadence', mode, 4)} | {interval('gui-current', mode, 4)} | {(1 - after / before) * 100:.2f}% |\n"
    maximum = max(r["slice_max_ns"]["max"] for r in SUMMARY if r["scenario"] == "gui-current") / 1e6
    text += f"\n新ループの最大呼出しは全採用GUIケースで{maximum:.3f}ms。ゲーム時刻は不変、858 hit / serial_rows=0、両周期でWorkと最終公開digestが一致した。\n"
    text += """
GUIのmain CPU raw値には0が出る試行があり、短いスライスとOS CPU時計の量子化の影響を受ける。その値からCPU増減は判断せず、上の復元・行構築の長い窓を使う。
実ウィンドウの長時間操作、複数PC間TCP通信の長時間試験は未実施。既存の同期barrier・進捗報告・timeout処理は変更せず、nativeのpacket handlerと参加状態テストで検査した。

## 証跡・再現

- [手順](passenger-preparation-benchmark.md)
- [全生値・集計・build identity](measurements/passenger-preparation-workers-2026-10-20/)
- [表構築・復元64本の採用検査](measurements/passenger-preparation-workers-2026-10-20/validation.json)、[GUI8本](measurements/passenger-preparation-workers-2026-10-20/gui/validation.json)
- [比較CSV](measurements/passenger-preparation-workers-2026-10-20/summary.csv)、[native/GUI検証](measurements/passenger-preparation-workers-2026-10-20/checks/tests.json)
- [exe/PDBの独立コピー・絶対パス・SHA-256](measurements/passenger-preparation-workers-2026-10-20/binary-backup.json)
- [競合で不採用にした試行と初期の診断fixtureの扱い](measurements/passenger-preparation-workers-2026-10-20/attempts/adoption.json)

基準TU、測定コマンド、seed、バイナリ、言語ファイル、変更ソースのhashを保管した。実マップsaveやETLは使用していない。
作業開始時のcheckoutにあったWPR手順書を読んでから準備したが、指定ベースのjgrpp-yaには同文書がないため、読んだcommit/pathをbuild identityへ記録した。
メイン残課題はLegacyのグラフ準備とHyperpathの行仕上げ・公開。旧行skipと保存方式の再設計は別段階とする。
"""
    report = DATA.parents[1] / "passenger-preparation-workers-2026-10-20.md"
    report.write_text(text, encoding="utf-8", newline="\n")
    print(report)


if __name__ == "__main__":
    main()
