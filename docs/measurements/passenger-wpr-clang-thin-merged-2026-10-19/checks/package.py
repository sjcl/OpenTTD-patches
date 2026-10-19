"""Verify exports, preserve raw recordings, and write the final measured report."""
from pathlib import Path
import gzip
import hashlib
import json
import re
import shutil
import stat

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-clang-thin-merged-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-loading-scratch-8f829b8c-2026-10-19'
BACKUP=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329')/'passenger-wpr-recordings-clang-thin-merged-2026-10-19'

def load(path):return json.loads(path.read_text(encoding='utf-8-sig'))
def dump(path,value):path.write_text(json.dumps(value,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def copy(source,target):target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)

assert (WORK/'analysis-complete.txt').is_file()
summary=load(OUT/'summary.json');old_summary=load(BASE/'summary.json');comparison=load(OUT/'comparison.json')
focus=load(OUT/'focus.json')
captures=comparison['capture_results'];validation={}
for index,mode in enumerate(['legacy','hyperpath'],1):
    s=summary[mode];assert s['lost_events']==s['lost_buffers']==0 and not s['unresolved_product_functions']
    assert s['payments_valid'] and s['ticks']==2048
    audit=load(WORK/mode/'window-audit.json');assert audit['valid']
    warnings=[int(t) for warning in s['wpa_invalid_event_warning'] for t in re.findall(r'TimeStamp:\s*(\d+)',warning)]
    assert all(t<audit['marker_begin_us'] or t>audit['marker_end_us'] for t in warnings)
    exported=load(OUT/mode/'export-command.json')
    for name,item in exported['exports'].items():
        with gzip.open(OUT/mode/(name+'.gz'),'rb') as stream:assert hashlib.file_digest(stream,'sha256').hexdigest()==item['sha256']
    for name in ['window-audit.json','window-markers.txt','window-processes.txt','recorder-markers.txt','recorder-trace-stats.txt']:
        copy(WORK/mode/name,OUT/mode/name)
    copy(WORK/'sampling-jobs'/f'{index:02}-{mode}'/'quality.json',OUT/mode/'recorder-quality.json')
    validation[mode]=dict(exact_ticks=2048,canonical=captures[mode]['canonical'],window_valid=True,lost_events=0,lost_buffers=0,
        raw_csv_hashes_match=True,unresolved_product_functions={},invalid_event_times_us=warnings,invalid_events_outside_markers=True,
        conservation_checks=s['conservation_checks'],payments_valid=True)
    assert all(item['equal'] for item in captures[mode]['canonical'].values())
tests=re.findall(r'Passed all (\d+) test cases with (\d+) assertions\.',(WORK/'native-all.log').read_text(errors='replace'))
assert len(tests)==1
dump(OUT/'validation.json',dict(product_commit='f97bac717c840852d8240f0eac51a92bad0abb9b',baseline_commit='8f829b8c3d5f35cb420c2a16812716ef24e38939',
     accepted_traces=2,native_tests=int(tests[0][0]),native_assertions=int(tests[0][1]),same_input=True,same_capture_profile=True,runs=validation))
for name in ['prepare.py','estimated_prepare_compat.py','record.ps1','measure.py','post.py','analyze.py','package.py','verify.py','finish.py','probe.py','check_build.py']:
    if (WORK/name).is_file():copy(WORK/name,OUT/'checks'/name)
for name in ['build-command.json','build-identity.json','input-identity.json','prepare-compat.json','capture-results.json','profile-interval.txt','recorder.log','native-all.log','configure.log','build.log','cpu-minimal.wprp','cpu-sampled.wpaProfile','export-runner.log','project-flags.json','lto-objects.json','manifest-verification.json','manifest.log','openttd.exe.manifest.xml','openttd_test.exe.manifest.xml','pdb-scope-check.txt','pdb-preflight.json','actual-compile-flags.json','merge.json']:
    copy(WORK/name,OUT/name)
for target in ['openttd_lib','openttd','openttd_test']:
    path=next((WORK/'compile'/(target+'.dir')/'Release').rglob('clang-cl.command.1.tlog'))
    (OUT/'checks'/(target+'-clang-command.tlog.gz')).write_bytes(gzip.compress(path.read_bytes(),mtime=0))
for name in ['package.json','openttd.cfg']:copy(WORK/'input'/name,OUT/'input'/name)
copy(BASE/'input-backup.json',OUT/'input-backup.json')
for name in ['quality.json','job.json']:copy(WORK/'sampling-jobs/00-pilot'/name,OUT/'checks/pilot'/name)
for name in ['recorder-markers.txt','recorder-trace-stats.txt']:copy(WORK/'pilot'/name,OUT/'checks/pilot'/name)
(OUT/'.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n**/*markers.txt -whitespace\n**/*trace-stats.txt -whitespace\n',encoding='utf-8',newline='\n')
BACKUP.mkdir()
backup=dict(path=str(BACKUP),files={},accepted_traces=2,profile_sha256=sha(WORK/'cpu-minimal.wprp'))
for name in ['Release/openttd.exe','Release/openttd.pdb','build-identity.json','legacy/cpu.etl','hyperpath/cpu.etl','legacy/result.sav','hyperpath/result.sav']:
    dest=BACKUP/name;copy(WORK/name,dest);assert sha(dest)==sha(WORK/name)
    dest.chmod(stat.S_IREAD);backup['files'][name]=dict(bytes=dest.stat().st_size,sha256=sha(dest),read_only=True)
dump(OUT/'recording-backup.json',backup)

labels=['StationCargoList::Load','Load -> _malloc_base','Load -> RtlFreeHeap','LoadingContext constructor','LoadingContext destructor',
        'Load -> LoadingContext constructor','Load -> LoadingContext destructor','MayUseHyperpathPolicy',
        'MayUseHyperpathPolicy -> PlanPrecomputed','CanUseHyperpathPolicy -> PlanPrecomputed','PlanPrecomputed','loading walk']
def pct(old,new):return 100*(new/old-1) if old else None
def table(mode):
    lines=['| 関数 / 経路 | 前回MSVC | 今回Clang ThinLTO | 生の変化 | samples 旧→新 |','| --- | ---: | ---: | ---: | ---: |']
    for label in labels:
        r=focus[mode][label];a=r['old'];b=r['new'];percent='—' if r['raw_percent'] is None else f"{r['raw_percent']:+.2f}%"
        if r.get('comparable') is False:
            lines.append(f"| `{label}` | {a['ms_per_tick']:.6f} | 比較不可 | — | {a['samples']:,} → — |")
        else:
            lines.append(f"| `{label}` | {a['ms_per_tick']:.6f} | {b['ms_per_tick']:.6f} | {percent} | {a['samples']:,} → {b['samples']:,} |")
    return '\n'.join(lines)
report=f'''# jgrpp-ya merge後のLLVM 23 ThinLTO実マップ計測

`sjcl/jgrpp-ya`の`82a587e2c6`を取り込み、merge commit `f97bac717c840852d8240f0eac51a92bad0abb9b`で、同じ固定済み`gc3833d807.sav`からLegacy・Hyperpathを各2,048 tick、WPRを各1本採取した（2026-10-19 JST）。
**終了canonical 24項目は両モードとも97cdc0ad・0b8a21c0・8f829b8cの同じモードと完全一致し、Hyperpathは6aeaff68のvalidateとも完全一致した。**

主比較は前回8f829b8cのMSVC 19.44 / assert ON / LTO OFFと、今回のmerge後LLVM 23.1.3 / assert OFF / ThinLTO。PGOは今回明示的にOFF。
**merge・コンパイラ・assert・LTO設定を合わせた差であり、ThinLTOやコンパイラ単独の効果を分離した比較ではない。**
コンパイラ変更は全コードへ効くため、`passenger_wpr_control.py`の「未変更部分」を機械変動の対照として使わず、生値を報告した。

## tick全体

時間はメインスレッドで`StateGameLoop`を含むWPR SampledProfileのCPU ms/tick。ロード・停止中の準備・保存は除外した。

| モード | sampled CPU 前回MSVC | 今回ThinLTO | 生の変化 | tick実時間 前回→今回 ms/tick |
| --- | ---: | ---: | ---: | ---: |
'''
for mode in ['legacy','hyperpath']:
    a=old_summary[mode];b=summary[mode];old_cpu=a['sampled_cpu_ms_per_tick']['simulation_stack'];new_cpu=b['sampled_cpu_ms_per_tick']['simulation_stack']
    report+=f"| {mode} | {old_cpu:.6f} | {new_cpu:.6f} | {pct(old_cpu,new_cpu):+.2f}% | {a['simulation_wall_ms_per_tick']:.6f} → {b['simulation_wall_ms_per_tick']:.6f} |\n"
report+=f'''
各組合せ1回であり、実行間の機械状態・測定ばらつきも含む。最適化・インライン化・ICFで関数境界が変わるため、全体の判断は上表を優先する。
以下の内訳はPDB上で解決された関数・呼出し経路のinclusive CPU ms/tick。0サンプルは処理時間0の証明ではなく、完全なインライン化等で呼出し境界が残らない場合もある。

## Hyperpathの内訳

{table('hyperpath')}

## Legacyの内訳

{table('legacy')}

`A -> B`は同じスタックでAの子孫にBがある経路だけを数えた。1スタック内の重複は1回だけ数え、親子inclusiveを加算していない。
ctor/dtorは全体とLoad配下を分けた。loading walkはCargoLoad用VisitBoardingCandidates本体の境界を用いる。
今回のVisitBoardingCandidates本体はインライン化され、ソース位置付きの内部lambdaが残っている。これらの関数名は解決できているが、旧CargoLoad本体と同じ境界を切り出せないため、loading walkは比較不可とした。

## 指定ビルド条件の検証

- ClangCL / LLD **23.1.3**、Visual Studio 2022 BuildToolsのMSVC ABI・STL・静的CRT、x64 Release。
- `openttd_lib`と`openttd`の実際のclang-clコマンドを検査し、両方で`-flto=thin /O2 /Ob2 /MT /Z7`、`NDEBUG`、PGO指定なしを確認した。CMakeの要求値だけで判断していない。
- `OPTION_USE_ASSERTS=OFF`、`OPTION_PGO=OFF`、`OPTION_NO_LTO=OFF`。自動IPOとの混在なし。専用ThinLTO backend cacheを使用。
- `openttd_lib`の608オブジェクトすべてがLLVM bitcode。exeおよびtestターゲットのbitcodeも確認した。詳細はlto-objects.json。
- compiler-rt builtinsの実リンク指定と実ファイルのSHA-256をactual-compile-flags.jsonに保存。
- `/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO`。SDKの`mt.exe`で元の`os/windows/openttd.manifest`をexe/testへ再埋め込みし、取り出した内容が元と等しいことを確認した。
- `/Z7`付きPDBのmodule symbols / inlinee IDsから`StateGameLoop`、`CanBoard`、`MayUseHyperpathPolicy`、`PlanPrecomputed`、`VisitBoardingCandidates`を採取前に確認。強制noinline等は追加していない。
- 全{int(tests[0][0]):,} nativeテスト・{int(tests[0][1]):,} assertions成功。

## memo・採取品質・保管

| モード | memo hits 前回→今回 | lookup rebuilds | indexed_groups |
| --- | ---: | ---: | ---: |
'''
for mode in ['legacy','hyperpath']:
    data=captures[mode];memo=data['diagnostics']['memo'];lookup=data['diagnostics']['lookup']
    report+=f"| {mode} | {data['baseline_memo']['hits']:,} → {memo['hits']:,} | {lookup['rebuilds']:,} | {lookup['indexed_groups']:,} |\n"
report+=f'''
- セーブSHA-256 `916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a`。cfg・245アセットも前回manifestへ照合。変換・再保存なし。
- StateTicks **3,197,276,448 → 3,197,278,496**。差・実サンプル数とも各2,048。canonicalは64bit digestを含めPython整数で照合した。
- `--resume --wait-saved-replay --precomputed --warm-precomputed --wait-background-ready --hyperpath-filter-mode index --main-thread-affinity-mask 1`。モードだけ切替。準備成功・準備中tick不変、cold・再構築・追加warm-up tickなし。
- 短いpilotが欠落0・両マーカーで成功した後、Legacy→Hyperpathを直列採取。kernel CPU専用File方式、1ms、1,024×1,024KiB。同じWPR/WPA profileをSHA-256で確認した。
- 両本ともlost events / buffers=0、両マーカーあり、kernel区間監査で競合ビルド・テスト・ゲームなし。WPAで製品関数の未解決なし。CSVハッシュ、時間・サンプル保存則、支払い整合性を確認。InvalidEvent警告は測定マーカー外。
- 通常indexモードの計測であり、追加のvalidateは行っていない。既存NewGRF警告とerror pause解除のログも保持した。
- ETL・exe/PDB・終了セーブは作業ツリー外へ独立コピーし、recording-backup.jsonへ場所・容量・SHA-256を記録。固定入力は既存ZIPを再利用した。

[全体集計](measurements/{NAME}/summary.json)、[関数別差分CSV](measurements/{NAME}/function-before-after.csv)、[注目経路](measurements/{NAME}/focus.json)、[canonical照合](measurements/{NAME}/canonical-comparison.json)、[実コンパイル指定](measurements/{NAME}/actual-compile-flags.json)、[PDB事前検証](measurements/{NAME}/pdb-preflight.json)、[品質検証](measurements/{NAME}/validation.json)。

生WPA CSVは各モード配下のgzip。checksのスクリプトは当日のパス・版を含む再現資料であり、その場所から直接実行する汎用runnerではない。
'''
report_path=ROOT/'docs'/(NAME+'.md');report_path.write_text(report,encoding='utf-8',newline='\n')
dump(OUT/'report-sha256.json',dict(path=report_path.relative_to(ROOT).as_posix(),sha256=sha(report_path)))
dump(OUT/'evidence-sha256.json',{p.relative_to(OUT).as_posix():dict(bytes=p.stat().st_size,sha256=sha(p)) for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='evidence-sha256.json'})
(WORK/'evidence-complete.txt').write_text('Merged LLVM ThinLTO report/evidence/backups complete\n',encoding='utf-8',newline='\n')
print('Evidence and report prepared: '+str(report_path),flush=True)
