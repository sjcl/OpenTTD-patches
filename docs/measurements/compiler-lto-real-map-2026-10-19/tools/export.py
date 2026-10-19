from pathlib import Path
import subprocess, sys, os, json, hashlib
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
BASE = ROOT / 'build-compiler-lto-comparison'
TOOLKIT = Path(r'C:\Users\bebe\.codex\worktrees\8621\OpenTTD-patches\build-wpr-9ab0228d\tools\wpt\Windows Kits\10\Windows Performance Toolkit')
names = sys.argv[1:] or ['wpr-msvc-1', 'wpr-clang-1', 'wpr-clang-2', 'wpr-msvc-2']
def sha(p):
    with p.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()
for name in names:
    directory = BASE / 'runs' / name
    compiler = name.split('-')[1]
    build = BASE / compiler
    env = dict(os.environ)
    env['_NT_SYMBOL_PATH'] = str(build / 'Release')
    env['_NT_SYMCACHE_PATH'] = str(build / 'symcache')
    env['_NT_EXECUTABLE_IMAGE_PATH'] = str(build / 'Release')
    (build / 'symcache').mkdir(exist_ok=True)
    output = directory / 'wpa'
    output.mkdir(exist_ok=True)
    subprocess.run([str(TOOLKIT / 'xperf.exe'), '-i', str(directory / 'cpu.etl'), '-o', str(directory / 'trace-stats.txt'), '-a', 'tracestats', '-timespan'], check=True)
    stats = (directory / 'trace-stats.txt').read_text(encoding='utf-8-sig')
    assert 'Total # Lost Buffers : 0' in stats and 'Total # Lost Events  : 0' in stats
    command = [str(TOOLKIT / 'wpaexporter.exe'), '-i', str(directory / 'cpu.etl'), '-symbols', '-marks', 'simulation-begin', 'simulation-end', '-profile', str(build / 'cpu-sampled.wpaProfile'), '-outputfolder', str(output), '-outputformat', 'CSV']
    print('Exporting', name, flush=True)
    with (directory / 'wpa-export.log').open('wb') as log:
        subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    identity = dict(command=command, environment={k: env[k] for k in ('_NT_SYMBOL_PATH', '_NT_SYMCACHE_PATH', '_NT_EXECUTABLE_IMAGE_PATH')}, etl_sha256=sha(directory/'cpu.etl'), pdb_sha256=sha(build/'Release/openttd.pdb'))
    (directory / 'export-command.json').write_text(json.dumps(identity, indent=2))
    print('Exported', name, flush=True)
