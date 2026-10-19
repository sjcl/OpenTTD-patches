"""Verify the completed default Windows Full LTO + PGO build and retain evidence."""
from pathlib import Path
import gzip
import hashlib
import json
import os
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
BUILD = ROOT / 'build-windows-default/release'
LOG = ROOT / 'build-windows-llvm-download/build-full-lto.log'
LLVM = Path(os.environ['ProgramFiles']) / 'LLVM/bin'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


log = LOG.read_text(encoding='utf-8', errors='replace')
assert '100% tests passed, 0 tests failed out of 784' in log
assert 'hash mismatch' not in log.lower()
commands = json.loads((BUILD / 'compile_commands.json').read_text())
game_commands = [c for c in commands if c['file'].endswith('.cpp') and any(
    t in c['output'] for t in ('openttd_lib.dir', 'openttd_test.dir', 'openttd.dir'))]
assert game_commands
for command in game_commands:
    flags = command['command']
    assert '-flto=full' in flags and '-flto=thin' not in flags
    assert '-fprofile-use=' in flags and '-fprofile-generate' not in flags
    assert 'WITH_PASSENGER_BENCHMARK' not in flags

metadata = json.loads((ROOT / 'cmake/pgo/windows.json').read_text())
assert sha(ROOT / 'cmake/pgo/windows.profdata') == metadata['profile_sha256']
selected = []
counts = {}
for filename, names in (
    ('openttd.cpp', ('StateGameLoop', 'GameLoop')),
    ('passenger_precomputed.cpp', ('Plan', 'PrepareGraph')),
):
    command = next(c for c in game_commands if c['file'].replace('\\', '/').endswith('/src/' + filename))
    selected.append(command)
    obj = BUILD / command['output']
    ir = ROOT / 'build-windows-checks' / ('full-lto-' + filename + '.ll')
    subprocess.run([str(LLVM / 'clang.exe'), '-S', '-emit-llvm', '-x', 'ir', str(obj), '-o', str(ir)], check=True)
    code = ir.read_text(encoding='utf-8')
    assert 'ProfileSummary' in code
    assert not re.search(r'!"ThinLTO", i32 1', code)
    for name in names:
        definition = next(line for line in code.splitlines() if line.startswith('define ') and ('?' + name + '@') in line)
        profile_id = re.search(r'!prof !(\d+)', definition)[1]
        count = re.search(r'^!' + profile_id + r' = !\{!"function_entry_count", i64 (\d+)', code, re.M)
        assert count and int(count[1]) > 0
        counts[name] = int(count[1])
assert counts['StateGameLoop'] == 4096
summary = dict(
    passed=True,
    base_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
    compiler=subprocess.check_output([str(LLVM / 'clang-cl.exe'), '--version'], text=True),
    lto='full', pgo='USE', profile_sha256=metadata['profile_sha256'],
    checked_compile_commands=len(game_commands), profile_entry_counts=counts,
    ctest_passed=784, ctest_seconds=float(re.search(r'Total Test time \(real\) = ([0-9.]+)', log)[1]),
    exe_sha256=sha(BUILD / 'openttd.exe'), pdb_sha256=sha(BUILD / 'openttd.pdb'),
    unit_exe_sha256=sha(BUILD / 'openttd_test.exe'),
    lto_module_sha256=sha(ROOT / 'cmake/LinkTimeOptimization.cmake'),
    runtime_architecture='x64', remote_actions_executed=False,
)
(OUT / 'verification.json').write_text(json.dumps(summary, indent=2) + '\n')
(OUT / 'compile-commands.json').write_text(json.dumps(selected, indent=2) + '\n')
with (OUT / 'build-and-ctest.log.gz').open('wb') as stream:
    with gzip.GzipFile(filename='', fileobj=stream, mode='wb', mtime=0) as compressed:
        compressed.write(LOG.read_bytes())
print(json.dumps(summary, indent=2))
