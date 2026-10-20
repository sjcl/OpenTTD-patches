"""Archive the accepted captures and separate validation, then write raw ABBA tables."""
from pathlib import Path
import gzip, hashlib, json, re, shutil, stat

ROOT=Path('C:/Users/bebe/.codex/worktrees/3abe/OpenTTD-patches')
NAME='passenger-wpr-abba-0d99a67f-2026-10-20'
WORK=ROOT/('build-wpr-abba-0d99a67f-2026-10-20')
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-abba-6c11c67f-2026-10-19'
BACKUP=Path('C:/Users/bebe/.codex/visualizations/2026/10/19/01a15484-1ab3-7172-aee8-1a55c6d1c3b4')/('passenger-wpr-recordings-abba-0d99a67f-2026-10-20')
ROUNDS=['01-A1','02-B1']
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def dump(p,x):p.write_text(json.dumps(x,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def copy(a,b):b.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(a,b)

assert (WORK/'analysis-complete.txt').is_file()
captures=load(WORK/'capture-results.json');focus=load(OUT/'focus.json');comparison=load(OUT/'comparison.json')
validate=load(WORK/'validate-result.json');failure=load(WORK/'canonical-failure.json')
validation={};counter=0
for name in ROUNDS:
    summary=load(OUT/name/'summary.json')
    for mode in ['legacy','hyperpath']:
        counter+=1;key=name+'/'+mode;run=WORK/name/mode;dest=OUT/name/mode;s=summary[mode]
        assert s['lost_events']==s['lost_buffers']==0 and not s['unresolved_product_functions'] and s['ticks']==2048 and s['payments_valid']
        audit=load(run/'window-audit.json');assert audit['valid']
        warnings=[int(t) for warning in s['wpa_invalid_event_warning'] for t in re.findall(r'TimeStamp:\s*(\d+)',warning)]
        assert all(t<audit['marker_begin_us'] or t>audit['marker_end_us'] for t in warnings)
        exported=load(dest/'export-command.json');assert exported['exit_code']==0
        for fn,item in exported['exports'].items():
            with gzip.open(dest/(fn+'.gz'),'rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==item['sha256']
        for fn in ['window-audit.json','window-markers.txt','window-processes.txt','recorder-markers.txt','recorder-trace-stats.txt']:
            copy(run/fn,dest/fn)
        copy(WORK/'sampling-jobs'/f'{counter:02}-{name}-{mode}'/'quality.json',dest/'recorder-quality.json')
        assert all(x['equal'] for x in captures[key]['canonical'].values()) == (key != '02-B1/hyperpath')
        validation[key]=dict(exact_ticks=2048,canonical=captures[key]['canonical'],window_valid=True,lost_events=0,lost_buffers=0,
            raw_csv_hashes_match=True,unresolved_product_functions={},invalid_event_times_us=warnings,invalid_events_outside_markers=True,
            conservation_checks=s['conservation_checks'],payments_valid=True)
tests=re.findall(r'Passed all (\d+) test cases with (\d+) assertions\.',(WORK/'native-all.log').read_text(errors='replace'));assert len(tests)==1
dump(OUT/'validation.json',dict(A_commit=comparison['A_commit'],B_commit=comparison['B_commit'],recorded_traces=4,canonical_valid_traces=3,abba_complete=False,canonical_equivalent=False,native_tests_B=int(tests[0][0]),
    native_assertions_B=int(tests[0][1]),same_input=True,same_capture_profile=True,runs=validation,separate_validate=validate))
for p in WORK.glob('*.py'):copy(p,OUT/'checks'/p.name)
copy(WORK/'record.ps1',OUT/'checks/record.ps1')
for fn in ['plan.json','build-command.json','build-identity.json','baseline-identity.json','input-identity.json','prepare-compat.json','capture-results.json',
    'capture-tool-identity.json','profile-interval.txt','recorder.log','native-all.log','configure.log','build.log','cpu-minimal.wprp','cpu-sampled.wpaProfile',
    'export-runner.log','project-flags.json','lto-objects.json','manifest-verification.json','manifest.log','openttd.exe.manifest.xml',
    'openttd_test.exe.manifest.xml','pdb-scope-check.txt','pdb-preflight.json','actual-compile-flags.json','validate-result.json','validate-runner.log','validate-controller.log','canonical-failure.json','after-runner.log','validate-canonical-comparison.json','post-partial-runner.log','analyze-partial-runner.log']:
    copy(WORK/fn,OUT/fn)
for target in ['openttd_lib','openttd','openttd_test']:
    p=next((WORK/'compile'/(target+'.dir')/'Release').rglob('clang-cl.command.1.tlog'))
    (OUT/'checks'/(target+'-clang-command.tlog.gz')).write_bytes(gzip.compress(p.read_bytes(),mtime=0))
for fn in ['package.json','openttd.cfg']:copy(WORK/'input'/fn,OUT/'input'/fn)
input_backup=load(BASE/'input-backup.json');assert sha(Path(input_backup['path']))==input_backup['sha256']
copy(BASE/'input-backup.json',OUT/'input-backup.json')
for fn in ['quality.json','job.json']:copy(WORK/'sampling-jobs/00-pilot'/fn,OUT/'checks/pilot'/fn)
for fn in ['recorder-markers.txt','recorder-trace-stats.txt']:copy(WORK/'pilot'/fn,OUT/'checks/pilot'/fn)
for fn in ['process.json','actual-ticks.json','save-integrity.json','on-1-console.txt','control.txt','on.cfg']:
    if (WORK/'validate'/fn).exists():copy(WORK/'validate'/fn,OUT/'validate'/fn)
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/*markers.txt -whitespace\n**/*trace-stats.txt -whitespace\n',encoding='utf-8',newline='\n')
BACKUP.mkdir(exist_ok=False)
backup=dict(path=str(BACKUP),files={},recorded_traces=4,abba_complete=False,profile_sha256=sha(WORK/'cpu-minimal.wprp'))
names=['Release/openttd.exe','Release/openttd.pdb','build-identity.json','baseline/Release/openttd.exe','baseline/Release/openttd.pdb','baseline/build-identity.json',
       'validate/result.sav']+[name+'/'+mode+'/'+fn for name in ROUNDS for mode in ['legacy','hyperpath'] for fn in ['cpu.etl','result.sav']]
for fn in names:
    p=WORK/fn;dest=BACKUP/fn;copy(p,dest);assert sha(dest)==sha(p)
    dest.chmod(stat.S_IREAD);backup['files'][fn]=dict(bytes=dest.stat().st_size,sha256=sha(dest),read_only=True)
dump(OUT/'recording-backup.json',backup)

def mean_table(mode,labels):
    lines=['| 対象 | A 平均 [範囲] | B 平均 [範囲] | B−A | 変化 | samples A/B |','| --- | ---: | ---: | ---: | ---: | ---: |']
    for label in labels:
        r=focus['comparison'][mode][label];a=r['A'];b=r['B'];pct='—' if r['raw_percent'] is None else f"{r['raw_percent']:+.2f}%"
        lines.append(f"| `{label}` | {a['mean']:.6f} [{a['minimum']:.6f}, {a['maximum']:.6f}] | {b['mean']:.6f} [{b['minimum']:.6f}, {b['maximum']:.6f}] | {r['delta_ms_per_tick']:+.6f} | {pct} | {a['samples'] if a['samples'] is not None else '—'} / {b['samples'] if b['samples'] is not None else '—'} |")
    return '\n'.join(lines)
def repetitions():
    lines=['| 順序 | 版 | モード | simulation CPU ms/tick | tick実時間 ms/tick | MayUse inclusive | Resolve inclusive | Board inclusive | Shared self |',
           '| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for key,r in focus['runs'].items():
        m=r['focus'];lines.append(f"| {key} | {r['label']} | {r['mode']} | {r['simulation_cpu_ms_per_tick']:.6f} | {r['simulation_wall_ms_per_tick']:.6f} | {m['MayUseHyperpathPolicy']['ms_per_tick']:.6f} | {m['ResolveHyperpath']['ms_per_tick']:.6f} | {m['Board']['ms_per_tick']:.6f} | {m['SharedHyperpathPlan self']['ms_per_tick']:.6f} |")
    return '\n'.join(lines)
d=validate['diagnostics']
def raw_table():
    lines=['| Hyperpath対象（CPU ms/tick） | A1 | B1 | samples A/B |','| --- | ---: | ---: | ---: |']
    for label in ['MayUseHyperpathPolicy','MayUseHyperpathPolicy self','ResolveHyperpath','BoardHyperpath','Board','ResolveHyperpath / Board union','SharedHyperpathPlan self','SharedHyperpathPlan','StateGameLoop']:
        r=focus['comparison']['hyperpath'][label];a=r['A'];b=r['B']
        lines.append(f"| `{label}` | {a['mean']:.6f} | {b['mean']:.6f} | {a['samples']} / {b['samples']} |")
    return '\n'.join(lines)
def diff_table():
    lines=['| canonical項目 | A1 Hyperpath | B1 Hyperpath |','| --- | ---: | ---: |']
    for key,(a,b) in failure['differences'].items():lines.append(f'| `{key}` | {a} | {b} |')
    return '\n'.join(lines)
report=f'''# 6c11c67f / 0d99a67f 計測：canonical不一致でABBA停止

A=`{comparison['A_commit']}`、B=`{comparison['B_commit']}`。2026-10-20 JST、同じ固定セーブからABBAを開始したが、**B1 Hyperpathでcanonical 24項目中11項目が不一致となり、手順書の基準でキューを停止した。ABBAは未完了で、性能改善の判定はできない。**

A1（Legacy→Hyperpath）とB1（Legacy→Hyperpath）の4本を保存した。A1の2本とB1 Legacyは、前回6c11c67fおよび今回A1の同じモードとcanonicalが一致した。B2とA2は未実行。不一致を消すための再実行・基準更新は行っていない。

## canonicalの差分

開始tick・終了tickと入力・exe/PDBの識別、復元と準備完了を確認した。B1 Hyperpathも2,048 tick、欠落0、競合負荷なしだが、結果の挙動が異なる。

{diff_table()}

[完全な差分](measurements/{NAME}/canonical-failure.json)、[4本の照合](measurements/{NAME}/canonical-comparison.json)。digestはPython整数で比較した。変更したキャッシュが原因か、その中のどの経路かは、この終了状態の比較だけでは断定していない。

## validate 1本（B、Hyperpath）

同じ固定入力から新規プロセスで`--hyperpath-filter-mode validate --validation-only`を1本だけ実行した。exact tick、replay・warm・背景処理の完了、準備中tick不変を確認した。性能集計には含めない。

| 診断 | validated | violations | lookup_duplicates |
| --- | ---: | ---: | ---: |
| memo | {d['memo']['validated']:,} | {d['memo']['violations']} | {d['memo']['lookup_duplicates']} |
| lookup | {d['lookup']['validated_groups']:,} | {d['lookup']['violations']} | — |

**診断カウンターの条件は合格した。** memoとlookupの検証件数は正で、violationsとlookup_duplicatesは0。\nただしvalidate終了canonicalもA1 Hyperpathと11項目が異なる。B1 indexのcanonicalとも異なる。[indexとの比較](measurements/{NAME}/validate-canonical-comparison.json)。\n`validate-result.json`の`passed=false`はcanonical条件を含む総合判定であり、違反カウンターが非0だったという意味ではない。
`simulation_timing_valid=false`はvalidation-onlyの意図した値。[判定・レシピ](measurements/{NAME}/validate-result.json)、[診断ログ](measurements/{NAME}/validate/on-1-console.txt)。

## 指定関数の診断用生値

**各版1本だけ、かつcanonicalが異なるため、以下を同じ挙動に対する速度改善と解釈しない。ABBA平均ではない。**
メインスレッドで`StateGameLoop`を含むSampledProfileのCPU ms/tick。`SharedHyperpathPlan self`はスタック末端だけ、ほかはinclusive。
同じスタック内の同じ関数は1回だけ数える。Resolve / Board unionはResolveHyperpath、BoardHyperpath、PassengerPrecomputedTable::Boardのいずれかを含むスタックの和集合で、親子を重複加算しない。
LTOで消えた関数の0サンプルは時間0の証明ではない。

{raw_table()}

4本の各値:

{repetitions()}

[指定関数CSV](measurements/{NAME}/focus-runs.csv)、[全関数CSV](measurements/{NAME}/function-before-after.csv)、[集計方法・未完了フラグ](measurements/{NAME}/comparison.json)。CSVのmean/min/maxは各版1本の同じ値であり、反復による範囲や信頼区間を表さない。untouched補正は行っていない。

## 条件・品質・保管

- A/BともClangCL / LLD 23.1.3、ThinLTO、assert OFF、PGO OFF、静的CRT、`/O2 /Ob2 /MT /Z7`、`NDEBUG`。実コマンド、bitcode、compiler-rt builtinsとmanifestを検証した。Aは前回の独立保存済みexe/PDBをハッシュ照合して再利用。Bは対象commitのソースから同じレシピで作成。
- Bの全{tests[0][0]} native tests、{int(tests[0][1]):,} assertions成功。計測フックはコピーTUのみで製品ソースは変更していない。対応PDBの内部関数名を事前確認。
- 固定セーブ46,620,248 bytes、SHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットも前回manifestと一致。StateTicks **3,197,276,448 → 3,197,278,496**、各本2,048 tick。
- `--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。準備完了、準備中tick不変。追加warm-up tick・cold・再構築指定なし。
- kernel CPU専用File profile、1ms、1,024×1,024KiB。pilot成功後に採取。4本すべて欠落events/buffers 0、両マーカーあり、区間監査で競合ゲーム・ビルド・テストなし。
- WPA製品関数の未解決0、時間・サンプル保存則、支払い整合性、gzip CSVと生CSVのハッシュを検証。InvalidEvent警告はマーカー外。
- 生ETL4本・終了セーブ5本・A/B exe/PDBを作業ツリー外へ独立コピーして読み取り専用化。固定入力は既存ZIPをハッシュ再照合。[保管manifest](measurements/{NAME}/recording-backup.json)、[入力ZIP](measurements/{NAME}/input-backup.json)。

[採取品質とcanonical・validateの独立判定](measurements/{NAME}/validation.json)。checksの採用スクリプトは`prepare.py`、`check_build.py`、`probe.py`、`measure.py`、`record.ps1`、`validate.py`、`post_partial.py`、`analyze_partial.py`、`package_partial.py`、`verify_partial.py`。`post.py` / `analyze.py` / `package.py` / `verify.py` / `after.py`は当初の8本完了用レシピで、途中停止後の集計・保管には使っていない。全て当日のパスを含む再現資料であり、その場所から実行する汎用ツールではない。
'''
path=ROOT/'docs'/(NAME+'.md');path.write_text(report,encoding='utf-8',newline='\n')
dump(OUT/'report-sha256.json',dict(path=path.relative_to(ROOT).as_posix(),sha256=sha(path)))
dump(OUT/'evidence-sha256.json',{p.relative_to(OUT).as_posix():dict(bytes=p.stat().st_size,sha256=sha(p)) for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='evidence-sha256.json'})
(WORK/'evidence-complete.txt').write_text('Four retained traces, one validation; canonical failure documented, ABBA incomplete\n',encoding='utf-8',newline='\n')
print(path,flush=True)
