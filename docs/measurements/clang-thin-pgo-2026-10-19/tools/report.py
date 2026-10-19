from pathlib import Path
import json,re
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'build-clang-thin-pgo'
report=ROOT/'docs/clang-thin-pgo-2026-10-19.md'
template=BASE/'report-template.md'
if not template.exists():template.write_text(report.read_text(encoding='utf-8'),encoding='utf-8')
text=template.read_text(encoding='utf-8')
plain=json.loads((BASE/'summary-plain.json').read_text());a,b=plain['runs']
delta=lambda x,y:(y/x-1)*100
outcome=f"ThinLTO Release に PGO を適用した。同じ1,024 tickの参考計測では world CPU が **{-delta(a['world_cpu_ms_per_tick'],b['world_cpu_ms_per_tick']):.1f}%減**、事前計算表の構築 CPU が **{-delta(a['build_cpu_seconds'],b['build_cpu_seconds']):.1f}%減**だった。\n775テスト・10,745,430 assertionsをすべて通過し、PGO 無効版とのマルチプレイも両方向で状態一致・desyncなしを確認した。"
identity=json.loads((BASE/'profiles/identity.json').read_text())
profile=json.loads((BASE/'profile-use-checks.json').read_text())
summary=(BASE/'profiles/summary.txt').read_text(encoding='utf-8-sig')
functions=re.search(r'Total functions: (\d+)',summary)[1]
details=f"採用プロファイル SHA-256: `{identity['profile_sha256']}`。\nIR プロファイルには {int(functions):,} 関数があり、world・経路探索・乗車判定の実行を確認した。\n最終 `openttd.obj` の world 関数 entry count は {profile['pgo']['openttd']['world_entry_count']:,}。代表3 object に ProfileSummary と function entry count が入り、PGO 無効版にはないことを確認した。\n最終版と無効版を `LLVM_PROFILE_FILE` 指定付きで実行しても raw profile を生成せず、最終版に学習用カウンターが残っていないことも確認した。"
lines=['各モード1回の参考測定。world CPU はコピー TU の `GetThreadTimes`（主スレッド user + kernel）を合計して1,024で割った値で、WPR の sampled CPU ではない。',
       '多くの tick が15.625 ms刻みの粗いカウンターであり、小さい差の判定には使用しない。ビルド・ユニットテスト・同期検証を性能計測と同時実行していない。',
       '', '| 指標 | ThinLTO | ThinLTO + PGO | 増減 |', '|---|---:|---:|---:|']
for label,key in [('world CPU (ms/tick・参考)','world_cpu_ms_per_tick'),('表構築 CPU (s)','build_cpu_seconds'),('表構築 wall (s)','build_wall_seconds')]:
    lines.append(f"| {label} | {a[key]:.4f} | {b[key]:.4f} | {delta(a[key],b[key]):+.2f}% |")
lines+=['', 'WPR の初回管理者起動は Windows が取り消した。ユーザー承認後の再起動要求はプロセス ID を返したが、その後の計測起動・収録状態確認が自動承認レビューの容量不足とタイムアウトにより未実行となり、収録開始を確認できていない。今回の**経路探索・乗車判定の個別 CPU 内訳は未取得**。壁時計の内訳を CPU 時間の代用にはしていない。',
        '同一セーブを同じ tick 区間で最後まで実行し、支払い情報を含む終了状態24項目が一致した。前回の MSVC LTO / Clang ThinLTO 比較の終了状態とも一致した。',
        '計測用コピー TU・同一入力に対する評価であり、別マップや GUI 描画への効果は判断していない。CPU affinity・電源設定は変更していない。']
if (BASE/'summary.json').exists():
    sampled=json.loads((BASE/'summary.json').read_text())
    assert not any(sampled['mismatches'].values())
    before,after=(sampled['medians'][mode] for mode in ('baseline','pgo'))
    world_before,world_after=(r['cpu_ms_per_tick']['world_ms'] for r in (before,after))
    outcome=f"ThinLTO Release に PGO を適用した。同じ1,024 tickの WPR 計測では world CPU が **{-delta(world_before,world_after):.1f}%減**、事前計算表の構築 CPU が **{-delta(before['build_cpu_seconds'],after['build_cpu_seconds']):.1f}%減**だった。\n775テスト・10,745,430 assertionsをすべて通過し、PGO 無効版とのマルチプレイも両方向で状態一致・desyncなしを確認した。"
    lines=['各モード1回、同じ1,024 tickを WPR で収録した。表構築は tick 区間の前に測定した。ビルド・ユニットテスト・同期検証は性能計測と同時実行していない。',
           '', '| 指標 | ThinLTO | ThinLTO + PGO | 増減 |', '|---|---:|---:|---:|']
    for label,key in [('world CPU (ms/tick)','world_ms'),('旅客経路探索 CPU (ms/tick)','route_ms'),('乗車判定 CPU (ms/tick)','boarding_ms'),('経路探索 ∪ 乗車判定 CPU (ms/tick)','route_or_boarding_ms')]:
        x,y=(r['cpu_ms_per_tick'][key] for r in (before,after))
        lines.append(f'| {label} | {x:.4f} | {y:.4f} | {delta(x,y):+.2f}% |')
    for label,key in [('表構築 CPU (s)','build_cpu_seconds'),('表構築 wall (s)','build_wall_seconds')]:
        x,y=before[key],after[key]
        lines.append(f'| {label} | {x:.4f} | {y:.4f} | {delta(x,y):+.2f}% |')
    lines+=['', 'world は主スレッドの `StateGameLoop` を含むスタックの sampled CPU。経路探索・乗車判定はその部分集合で、対象関数の子孫を含む inclusive CPU。和集合では重複を一度だけ数えた。',
            '対象関数一覧と生の集計を `summary.json` に保存した。PGO によるインライン化はスタックの帰属にも影響するため、個別項目の差は world 全体の差と合わせて読む。',
            '両トレースとも lost buffers / lost events は0。各1回・単一マップの比較であり、学習区間と比較区間は重なる。別マップや GUI 描画への効果、統計的な有意差は判断していない。CPU affinity・電源設定は変更していない。',
            '', f"WPR を使用しない参考測定では world の粗い GetThreadTimes カウンターは {a['world_cpu_ms_per_tick']:.4f} → {b['world_cpu_ms_per_tick']:.4f} ms/tick、表構築 CPU は {a['build_cpu_seconds']:.4f} → {b['build_cpu_seconds']:.4f} s。15.625 ms刻みになるカウンターであり、WPR の値とは測定方法が異なる。",
            '両計測で、同じ tick の終了状態24項目（支払い情報を含む）が一致し、前回の MSVC LTO / Clang ThinLTO 比較の終了状態とも一致した。']
mp=['### マルチプレイ', '', '| サーバー | クライアント | 開始 tick | 終了 tick | 実行 tick 数 | 結果 |', '|---|---|---:|---:|---:|---|']
for server,client in [('baseline','pgo'),('pgo','baseline')]:
    r=json.loads((BASE/f'mp-{server}-{client}/result.json').read_text())
    assert r['pass'] and r['server_snapshot']==r['client_snapshot'] and r['transport_server']==r['transport_client']
    mp.append(f"| {server} | {client} | {r['server_snapshot']['state_ticks']} | {r['transport_server']['state_ticks']} | {r['actual_ticks']} | 状態一致・desyncなし |")
mp+=['', '同一セーブをネイティブネットワーク経由で転送し、各4 worker・frame frequency 1で各256 tickを要求した。',
     '2秒間隔の監視と停止伝播による超過を含む実行 tick 数を示した。各組の同じ終了 tick で比較し、方向間の異なる終了 tick の状態同士は比較していない。',
     '接続直後の DB 再生は tick を進めず完了し、接続直後・走行後とも両 peer が一致した。これはこのセーブ・この区間での確認である。']
artifacts='最終実行ファイル: `build-clang-thin-pgo/pgo/Release/openttd.exe`。プロファイル: `build-clang-thin-pgo/profiles/real-map.profdata`。\n[証跡](measurements/clang-thin-pgo-2026-10-19/README.md) にビルド指定、raw / indexed profile の識別情報、圧縮 `.profdata`、bitcode 検査、単体テスト・再構成テスト、tick 区間、状態・同期検証結果を保存した。\n実行ファイル・PDB・raw profile・素材・セーブは ignored の build ディレクトリに保持する。'
for key,value in [('OUTCOME',outcome),('PROFILE',details),('RESULTS','\n'.join(lines)),('MULTIPLAYER','\n'.join(mp)),('ARTIFACTS',artifacts)]:
    assert '<!-- '+key+' -->' in text
    text=text.replace('<!-- '+key+' -->',value)
report.write_text(text,encoding='utf-8')
print('Wrote',report)
