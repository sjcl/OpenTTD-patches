"""Render the report from adopted native readiness records, without simulation ticks."""
from prepare import ROOT, WORK, EVIDENCE, BASE, NEW, dump
from pathlib import Path
import json

def read(p): return json.loads(p.read_text(encoding='utf-8-sig'))
def ready(path): return read(WORK / 'waiting' / path / 'waiting-ready.json')
def seconds(value): return value / 1e9
def change(a, b): return (b / a - 1) * 100

cases = [
    ('初期構築 Legacy', 'initial/A2/legacy', 'initial/B1/legacy'),
    ('初期構築 Hyperpath', 'initial/A1/hyperpath', 'initial/B1/hyperpath'),
    ('旧世代を含む復元 Legacy', 'replay/A1/legacy', 'replay/B1/legacy'),
    ('旧世代を含む復元 Hyperpath', 'replay/A1/hyperpath', 'replay/B1/hyperpath'),
    ('TCP参加 Hyperpath', 'multiplayer-clean/baseline/client', 'multiplayer-clean/candidate/client')]
rows = []
for label, a, b in cases:
    av, bv = ready(a), ready(b)
    rows.append({'case': label, 'baseline': av, 'candidate': bv, 'baseline_path': a, 'candidate_path': b,
        'preparation_change_percent': change(av['preparation_wall_ns'], bv['preparation_wall_ns']),
        'process_to_ready_change_percent': change(av['process_to_ready_ns'], bv['process_to_ready_ns'])})
dump(EVIDENCE / 'summary.json', {'comparisons': rows,
    'workers8': {kind: ready(kind + '/workers8/hyperpath') for kind in ['initial', 'replay']},
    'supplemental': {'legacy_baseline_first_native': ready('initial/A1/legacy'), 'hyperpath_candidate_pilot': ready('initial/pilot/hyperpath')},
    'statistics': 'Primary cases are one fresh process per version/mode. No confidence interval or cross-machine guarantee.'})

lines = ['# 実マップのロード・途中参加後の待機時間（2026-10-20）', '',
    f'`g1c42420af.sav`で、基準 `{BASE}` と今回の `{NEW}` を比較した。通常tickはユーザーの指定により対象外とし、採用した全待機計測で **StateTicks=3,201,034,745のまま** 準備を完了した。製品コードの追加変更はない。', '',
    '既存の旧世代復元が直列だったため、GUIの27ms周期による待ちも重なっていた。今回の4 worker設定では、Legacyの復元待ちが約225秒から26秒、Hyperpathが169秒から14秒へ短縮した。実TCP参加の起動から初回同期完了までは185秒から24秒へ短縮した。', '',
    '以下の「旅客準備待ち」は、最初の`PreparePrecomputed` / `StepLoad`から完了までの実時間で、GUI周期間の待ちを含む。「起動→完了」はプロセス作成からの時刻で、アセット初期化、ファイル読み込み、参加時の転送も含む。起動済みアプリのボタンクリックだけの時間ではない。参加完了は製品の初回syncが成功し、参加ダイアログを閉じる箇所で記録した。', '',
    '| 条件 | 旅客準備待ち 基準→今回 秒 | 変化 | 起動→完了 基準→今回 秒 |',
    '| --- | ---: | ---: | ---: |']
for r in rows:
    a, b = r['baseline'], r['candidate']
    lines.append(f"| {r['case']} | {seconds(a['preparation_wall_ns']):.3f} → {seconds(b['preparation_wall_ns']):.3f} | {r['preparation_change_percent']:+.2f}% | {seconds(a['process_to_ready_ns']):.3f} → {seconds(b['process_to_ready_ns']):.3f} |")
lines += ['', '要求worker数は両版とも4。初期構築の実worker数はLegacyが3（既存メモリ予約上限による制限）、Hyperpathが4だった。以下のコア相当は、`pax-destination`という名前のworkerだけのOS CPU時間合計を準備実時間で割った値。全32論理CPUに対する使用率ではない。プロセスCPUには描画やロードしたCargoDist背景計算も含むため、worker CPUと混同しない。', '',
    '| 条件 | worker CPU 基準→今回 秒 | worker平均稼働 基準→今回 コア相当 | main CPU 基準→今回 秒 |',
    '| --- | ---: | ---: | ---: |']
for r in rows:
    a, b = r['baseline'], r['candidate']
    lines.append(f"| {r['case']} | {seconds(a['worker_cpu_ns']):.3f} → {seconds(b['worker_cpu_ns']):.3f} | {a['worker_cpu_ns']/a['preparation_wall_ns']:.2f} → {b['worker_cpu_ns']/b['preparation_wall_ns']:.2f} | {seconds(a['main_cpu_ns']):.3f} → {seconds(b['main_cpu_ns']):.3f} |")
lines += ['', '旧世代復元では基準版のworker CPUは0だった。今回Hyperpathは約3.9コア相当で、4 workerをほぼ継続稼働させている。初期構築でも今回の全4,936行がworker hit、serial_rows=0となった。Legacyの残る待ちは、復元のグラフ準備約6.7秒と、旧世代行の復元約19.6秒に分かれる。復元時の旧世代worker数そのものはこのreadiness記録に含めていない。', '',
    '| Hyperpathの今回版 | worker 4 準備待ち 秒 | worker 8 準備待ち 秒 | worker 8 起動→完了 秒 | worker 8 平均稼働 |',
    '| --- | ---: | ---: | ---: | ---: |']
for kind, label in [('initial', '初期構築'), ('replay', '旧世代復元')]:
    four, eight = ready(kind + '/B1/hyperpath'), ready(kind + '/workers8/hyperpath')
    lines.append(f"| {label} | {seconds(four['preparation_wall_ns']):.3f} | {seconds(eight['preparation_wall_ns']):.3f} | {seconds(eight['process_to_ready_ns']):.3f} | {eight['worker_cpu_ns']/eight['preparation_wall_ns']:.2f}コア相当 |")
lines += ['', 'peak working setは起動・読み込みも含むプロセス全体の最大値で、準備窓だけの増分ではない。', '',
    '| 条件 | 基準 peak GiB | 今回 peak GiB |', '| --- | ---: | ---: |']
for r in rows:
    a = read(WORK / 'waiting' / r['baseline_path'] / 'observed.json')['peak']['peak_working_bytes']
    b = read(WORK / 'waiting' / r['candidate_path'] / 'observed.json')['peak']['peak_working_bytes']
    lines.append(f"| {r['case']} | {a / 2**30:.3f} | {b / 2**30:.3f} |")
for kind, label in [('initial', '初期構築 Hyperpath worker 8'), ('replay', '旧世代復元 Hyperpath worker 8')]:
    b = read(WORK / 'waiting' / kind / 'workers8/hyperpath/observed.json')['peak']['peak_working_bytes']
    lines.append(f'| {label} | — | {b / 2**30:.3f} |')
lines += ['', '8 workerではHyperpathの復元が約7.1秒となり、worker CPUは約7.7コア相当（8本あたり約96.5%）だった。この補足は同じ版のローカル設定だけを変えた試験で、8 workerでのTCP参加時間は測っていない。ユーザーの通常設定ファイルは変更していない。', '',
    '入力は48,312,608 bytes、SHA-256 `22743c39125f5ac35a813144506040f3693b40d0f374514a0bb8c3c20eed9bb7`。元ファイルをコピーしてcfg・全アセットを凍結した。元セーブは内部形式v1なので、両版とも航空機コスト用の移動／待ち分離情報を一度再取得する。この初回ロードの初期構築と、v2保存状態の旧世代復元を分けて測った。', '',
    '復元用の共通入力は同じマップを0 tickのまま構築し、乗換ペナルティ20→17で世代を切り替えて保存した。旧世代4,936行、新世代0公開行・7 Workを保持する。両版の完了時load_workはLegacy **5,806,592,131**、Hyperpath **1,973,842,621**で一致し、保存済み新世代Workも7のまま。不要行skipやWork上限の変更による短縮ではない。', '',
    'TCP参加は同一PCの実暗号化loopback接続、専用サーバー＋Windows GUI観戦クライアントで行った。基準版同士／今回版同士を接続し、ホストをポーズしたまま参加した。参加用コピーには保存済みのErrorポーズがあり、最初の専用サーバーは製品の判定で拒否した。既存の計測用`unpause`で解除して0 tick再保存した入力を両版で使用し、解除前後のcanonical全24項目一致を確認した。元セーブと元の復元用コピーは保持し、専用サーバーの拒否判定やNewGRFを改変していない。NewGRFの容量変更警告はログに残っている。', '',
    'Windows / ClangCL 23.1.3、x64 Release `/O2 /Ob2 /MT /Zi`、assert ON、LTO OFF、PGO OFF。実Win32 GDIドライバの既定の別ゲームスレッドを使用し、640×480・非表示で同じGUI周期を実行した。描画負荷や初期化量は普段の画面サイズと異なり、CPU・回線が異なる別PCへの性能保証ではない。各本は新規プロセス、OSファイルキャッシュは消去していない。主比較は各1本なので、小差の有意性は主張しない。', '',
    '初期Legacyの最初のnative値は35.704秒、外部監視が揃った追加試行は38.191秒だった。最初は終了を先に検出したPython監視側の競合で最終census/peak metadataが残らず、生のready/canonicalは保持した。以降は終了後もファイルを読み、監視記録を随時保存するよう修正した。表のLegacy基準は追加試行を使用する。今回Hyperpathのpilotは12.800秒、主比較は12.427秒だった。失敗や別条件を混ぜて平均していない。', '',
    '最長の準備API呼出しはLegacy初期で基準37.9ms／今回85.0ms、Legacy復元で98.6ms／51.7ms。Hyperpath復元は9.4ms／13.1ms、8 workerは26.8msだった。これはOSの中断も含む壁時計であり、GUI全体のフレーム時間ではない。外側8msの目標は単一API呼出しの厳密な上限ではなく、大きな割当・グラフ準備などの応答遅延は残る。', '',
    '採用した全ケースで時刻不変、版間・worker数間のcanonical **24項目一致**、実参加ではホスト／クライアント間も24項目一致。最終計測バイナリのnativeは基準801ケース／10,757,738 assertions、今回805ケース／10,523,428 assertionsが成功した。通常tickのfilter validateは対象外・未実施で、canonical一致をvalidate成功と呼んでいない。', '',
    '当初の通常tick採取はユーザーの対象変更で追加を停止し、完了済み2本も今回の性能結論から除外した。WPR pilotは検証済みkernel専用profileで欠落0・両マーカー・順序を確認したが、ここで示す待機時間とCPUはGUI／初回syncのマーカーとOS CPU時計による値であり、2,048 tickのCPUサンプル値ではない。', '',
    '入力ZIP・派生セーブ・両版のexe/PDB/lang・使ったfixture生成exe/PDB・pilotと除外ETLは、作業ツリー外の `C:\\Users\\bebe\\OpenTTD-measurements\\passenger-waiting-real-20261020-fe663cfa` に独立コピーで保管し、SHA-256を記録した。元セーブのhash不変とZIP内の全アセットも照合した。', '',
    '- [全生値・CPU・フェーズ](measurements/passenger-preparation-real-g1c42420af-2026-10-20/summary.json)',
    '- [採用・native結果](measurements/passenger-preparation-real-g1c42420af-2026-10-20/validation.json)、[canonical照合](measurements/passenger-preparation-real-g1c42420af-2026-10-20/canonical-comparisons.json)',
    '- [入力manifest](measurements/passenger-preparation-real-g1c42420af-2026-10-20/input/package.json)、[独立保管manifest](measurements/passenger-preparation-real-g1c42420af-2026-10-20/backup-manifest.json)',
    '- [再現用スクリプト](measurements/passenger-preparation-real-g1c42420af-2026-10-20/checks/)、[不採用・失敗の扱い](measurements/passenger-preparation-real-g1c42420af-2026-10-20/attempts.json)',
    '']
(ROOT / 'docs/passenger-waiting-real-g1c42420af-2026-10-20.md').write_text('\n'.join(lines), encoding='utf-8', newline='\n')
print('Report and summary generated', flush=True)
