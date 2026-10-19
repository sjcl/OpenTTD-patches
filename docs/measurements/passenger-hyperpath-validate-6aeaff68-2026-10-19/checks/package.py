"""Preserve the single validation's evidence and immutable executable/PDB backup."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import stat
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = Path(__file__).resolve().parent
NAME = 'passenger-hyperpath-validate-6aeaff68-2026-10-19'
OUT = ROOT / 'docs/measurements' / NAME
BASE = ROOT / 'docs/measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
BACKUP = Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329') / NAME

def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

assert (WORK / 'validate-complete.txt').is_file()
result = load(WORK / 'validate-result.json')
meta = load(WORK / 'validate/process.json')
native = (WORK / 'native-all.log').read_text(encoding='utf-8', errors='replace')
tests = re.findall(r'Passed all (\d+) test cases with (\d+) assertions\.', native)
assert len(tests) == 1
test_info = dict(test_cases=int(tests[0][0]), assertions=int(tests[0][1]), passed=True)
dump(WORK / 'native-test.json', test_info)
OUT.mkdir()
(OUT / 'checks').mkdir()
(OUT / 'validate').mkdir()
(OUT / 'input').mkdir()
(OUT / '.gitattributes').write_text('* -text whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol\n*.log -whitespace\n', encoding='utf-8')
for name in ['input-identity.json', 'build-command.json', 'build-identity.json', 'prepare-compat.json',
             'validate-command.json', 'validate-result.json', 'native-test.json', 'native-all.log',
             'configure.log', 'build.log', 'validate-runner.log']:
    shutil.copy2(WORK / name, OUT / name)
shutil.copy2(WORK / 'input/package.json', OUT / 'input/package.json')
shutil.copy2(WORK / 'input/openttd.cfg', OUT / 'input/openttd.cfg')
shutil.copy2(BASE / 'input-backup.json', OUT / 'input-backup.json')
archive = load(OUT / 'input-backup.json')
assert Path(archive['path']).is_file() and sha(Path(archive['path'])) == archive['sha256']
for name in ['process.json', 'actual-ticks.json', 'save-integrity.json', 'on-1-console.txt', 'stderr.txt', 'stdout.txt']:
    shutil.copy2(WORK / 'validate' / name, OUT / 'validate' / name)
for name in ['prepare.py', 'estimated_prepare_compat.py', 'validate.py', 'package.py']:
    shutil.copy2(WORK / name, OUT / 'checks' / name)
instrumentation = {}
for folder in ['save-integrity', 'real-map-driver']:
    for path in sorted((WORK / folder).glob('*')):
        if path.is_file():
            instrumentation[path.relative_to(WORK).as_posix()] = dict(bytes=path.stat().st_size, sha256=sha(path))
dump(OUT / 'instrumentation-sha256.json', instrumentation)
BACKUP.mkdir()
backup = dict(path=str(BACKUP), files={}, wpr=False, run_count=1)
for name in ['Release/openttd.exe', 'Release/openttd.pdb', 'build-identity.json', 'validate/result.sav']:
    target = BACKUP / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(WORK / name, target)
    assert sha(target) == sha(WORK / name)
    target.chmod(stat.S_IREAD)
    backup['files'][name] = dict(bytes=target.stat().st_size, sha256=sha(target), read_only=True)
dump(OUT / 'recording-backup.json', backup)
lookup, memo = result['lookup'], result['memo']
canonical_equal = all(item['equal'] for item in result['canonical'].values())
report = f'''# 6aeaff68: 実マップHyperpath validate

同じ固定済み `gc3833d807.sav` を使い、`{result['product_commit']}` でHyperpathのvalidateを**1本、2,048 tick**実行した（2026-10-19 JST）。

**検証{'合格' if result['validation_passed'] else '不合格'}: lookup violations={lookup['violations']:,}、memo violations={memo['violations']:,}、lookup_duplicates={memo['lookup_duplicates']:,}。**
終了canonical 24項目は、前回0b8a21c0のvalidate・Hyperpath通常実行、および97cdc0adのHyperpathと{'すべて完全一致した' if canonical_equal else '不一致あり（詳細JSON参照）'}。
JSONの整数として比較し、64bit digestを浮動小数点へ変換していない。

```text
Passenger hyperpath lookup: {result['lookup_line']}
Passenger hyperpath memo: {result['memo_line']}
```

| 検証項目 | 件数 |
| --- | ---: |
| lookup validated_groups | {lookup['validated_groups']:,} |
| lookup violations | {lookup['violations']:,} |
| memo validated | {memo['validated']:,} |
| memo conservative | {memo['conservative']:,} |
| memo violations | {memo['violations']:,} |
| lookup_duplicates | {memo['lookup_duplicates']:,} |

前回0b8a21c0ではmemo validated=13,769,660、violations=13,655だった。
6aeaff68では、保存済み回答と再計算が違うとき、失敗系統のlocal policyがunknownならexact回答で追加照合し、保存済み回答と同じならconservativeへ数える。今回のconservativeは{memo['conservative']:,}件。
uniqueグループの重複も独立したlookup_duplicatesで確認した。新版では確定したtrue回答も保存するため、validated件数も旧版と単純には対応しない。

条件と確認:

- 入力セーブSHA-256 `{result['input_sha256']}`。cfg・245アセットを前回manifestと照合し、変換・再保存せずに使用。
- StateTicks {result['start_tick']:,} → {result['end_tick']:,}。実tick差とtickサンプル数はいずれも2,048。
- `--resume --wait-saved-replay --precomputed --hyperpath --warm-precomputed --wait-background-ready --hyperpath-filter-mode validate --main-thread-affinity-mask 1`。
- saved replay完了、warm成功、背景処理完了。準備前後のStateTicksは固定入力の開始tickから変化なし。cold・再構築条件・追加のwarm-up tickなし。
- 指定commitへ一時的にcheckoutし、MSVC 19.44 x64 Release、assert ON、LTO OFF、静的CRT、前回と同じフラグと計測用コピーでビルド。製品コードの追加変更なし。
- 全{test_info['test_cases']:,} nativeテスト・{test_info['assertions']:,} assertionsが通った。
- **正しさ専用の `--validation-only`、WPRなし。** 別作業のPGO処理を停止せず実行し、重複プロセスはprocess.jsonに保存した。`simulation_timing_valid=false` はこの指定による意図した値で、所要時間を性能比較には使っていない。
- セーブ既存のNewGRF警告とerror pause解除の記録はコンソール・stderrに保持。終了時支払い整合性もtrue。

[検証結果](measurements/{NAME}/validate-result.json)、[コンソール全文](measurements/{NAME}/validate/on-1-console.txt)、[終了canonical](measurements/{NAME}/validate/save-integrity.json)、[実行metadata](measurements/{NAME}/validate/process.json)、[ビルド同一性](measurements/{NAME}/build-identity.json)。

exe/PDBと終了セーブは作業ツリー外に保存し、[バックアップmanifest](measurements/{NAME}/recording-backup.json)へ場所とハッシュを記録した。
固定入力は既存の同一ZIPを再利用し、[入力バックアップmanifest](measurements/{NAME}/input-backup.json)のSHA-256を確認した。
採用した補助スクリプトはchecks配下に保存している。当日のパスとrevisionを含む再現資料であり、その場所から直接実行する汎用runnerではない。
'''
report_path = ROOT / 'docs' / (NAME + '.md')
report_path.write_text(report, encoding='utf-8', newline='\n')
dump(OUT / 'report-sha256.json', dict(path=report_path.relative_to(ROOT).as_posix(), sha256=sha(report_path)))
inventory = {p.relative_to(OUT).as_posix(): dict(bytes=p.stat().st_size, sha256=sha(p))
             for p in sorted(OUT.rglob('*')) if p.is_file()}
dump(OUT / 'evidence-sha256.json', inventory)
print(json.dumps(dict(report=str(report_path), evidence_files=len(inventory)+1,
                     validation_passed=result['validation_passed'], canonical_equal=canonical_equal), indent=2), flush=True)
