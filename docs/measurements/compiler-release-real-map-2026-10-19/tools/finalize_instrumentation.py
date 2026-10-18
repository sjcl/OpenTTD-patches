from pathlib import Path
import subprocess, sys
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
BASE = ROOT / 'build-compiler-comparison'
for compiler in ('msvc', 'clang'):
    build = BASE / compiler
    driver = build / 'real-map-driver/null_v.cpp'
    timed = driver.read_text(encoding='utf-8')
    start = timed.index('\t} else {', timed.index('void VideoDriver_Null::MainLoop()'))
    end = timed.index('\n\t/* If requested', start)
    timed_branch = timed[start:end]
    subprocess.run([sys.executable, str(ROOT / 'utils/passenger_precomputed_join_prepare.py'), str(ROOT), str(build), 'a5aa133cfe51cd39ab9175396f318824af97b30e'], check=True)
    join = driver.read_text(encoding='utf-8')
    start = join.index('\t} else {', join.index('void VideoDriver_Null::MainLoop()'))
    end = join.index('\n\t/* If requested', start)
    combined = join[:start] + timed_branch + join[end:]
    combined = combined.replace('#include <atomic>', '#include <atomic>\n#include "' + (ROOT / 'src/passenger_journey_active.h').as_posix() + '"')
    driver.write_text(combined, encoding='utf-8')
for script in ('prepare_cpu.py', 'prepare_queries.py'):
    subprocess.run([sys.executable, str(BASE / script)], check=True)
