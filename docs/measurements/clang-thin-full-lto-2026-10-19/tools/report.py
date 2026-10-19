from pathlib import Path
import json

ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'build-clang-lto-comparison'
data=json.loads((BASE/'summary.json').read_text())
med=data['medians']
items=[('world tick 全体 CPU','world_ms'),('旅客経路探索 CPU','route_ms'),('乗車判定 CPU','boarding_ms'),('経路探索 ∪ 乗車判定 CPU','route_or_boarding_ms')]
lines=['有効試行は ThinLTO 1回、Full LTO 2回。以下は各モードの中央値（Thin は単一値）。CPU は小さいほど軽く、増減は Full / Thin − 1。',
       '', '| 指標 | ThinLTO | Full LTO | Full の増減 |', '|---|---:|---:|---:|']
for label,key in items:
    a,b=(med[m]['cpu_ms_per_tick'][key] for m in ('thin','full'))
    lines.append(f'| {label} (ms/tick) | {a:.4f} | {b:.4f} | {(b/a-1)*100:+.2f}% |')
for label,key in [('事前計算表の構築 CPU (s)','build_cpu_seconds'),('事前計算表の構築 wall (s)','build_wall_seconds')]:
    a,b=(med[m][key] for m in ('thin','full'))
    lines.append(f'| {label} | {a:.4f} | {b:.4f} | {(b/a-1)*100:+.2f}% |')
lines+=['', '| 試行順 | world CPU (ms/tick) | 経路探索 CPU (ms/tick) | 乗車判定 CPU (ms/tick) | 構築 CPU (s) | 構築 wall (s) |', '|---|---:|---:|---:|---:|---:|']
for run in data['runs']:
    cpu=run['per_tick_cpu_ms']
    lines.append(f"| {run['name']} | {cpu['world_ms']:.4f} | {cpu['route_ms']:.4f} | {cpu['boarding_ms']:.4f} | {run['build_cpu_seconds']:.4f} | {run['build_wall_seconds']:.4f} |")
lines+=['', '経路探索と乗車判定は重複する inclusive 範囲であり、和集合を別記した。', '表構築は全 worker の CPU 合計であり、wall 時間より大きくなる。']
perf='\n'.join(lines)
checks=[f"採用した3試行すべて終了コード0、連続する1,024 tickを確認。終了時の{len(data['canonical']['wpr-thin-1'])}項目はすべて一致し、支払い情報も正常。前回の MSVC LTO / Clang ThinLTO 比較の終了状態とも一致した。",
        'WPR の lost buffers / lost events は採用した3試行とも0。', '',
        '| サーバー | クライアント | 開始 tick | 終了 tick | 実行 tick 数 | 結果 |', '|---|---|---:|---:|---:|---|']
for server,client in [('thin','full'),('full','thin')]:
    r=json.loads((BASE/f'mp-{server}-{client}/result.json').read_text())
    assert r['pass'] and r['server_snapshot']==r['client_snapshot'] and r['transport_server']==r['transport_client']
    checks.append(f"| {server} | {client} | {r['server_snapshot']['state_ticks']} | {r['transport_server']['state_ticks']} | {r['actual_ticks']} | 接続・状態一致、desync なし |")
checks+=['', '同一セーブをネイティブのネットワーク経由で転送し、サーバー・クライアント各4 worker、frame frequency 1で確認した。',
         '各方向256 tickを要求し、2秒間隔の監視と停止伝播による超過を含む実測区間を上表に示した。各組の同一終了 tick で状態を比較し、方向間で終了 tick が異なる場合はその終了状態同士を比較していない。',
         '接続直後の保存済み DB 再生は tick を進めず完了し、接続直後・走行後ともサーバーとクライアントの状態が一致した。これはこのセーブ・この短い区間での検証である。']
warnings={r['warning_lines'] for r in data['runs']}
resumes={r['initial_error_resumes'] for r in data['runs']}
assert len(warnings)==len(resumes)==1 and all(r['error_resumes']==0 for r in data['runs'])
checks+=['',f'入力 NewGRF 由来の警告は各試行{next(iter(warnings))}行で同一。準備時のエラーポーズ解除は各{next(iter(resumes))}回、計測 tick 内の追加解除は0回だった。']
path=ROOT/'docs/clang-thin-full-lto-2026-10-19.md'
text=path.read_text(encoding='utf-8')
assert '<!-- PERFORMANCE_RESULTS -->' in text and '<!-- MULTIPLAYER_RESULTS -->' in text
text=text.replace('<!-- PERFORMANCE_RESULTS -->',perf).replace('<!-- MULTIPLAYER_RESULTS -->','\n'.join(checks))
path.write_text(text,encoding='utf-8')
print('Report filled:',path)
