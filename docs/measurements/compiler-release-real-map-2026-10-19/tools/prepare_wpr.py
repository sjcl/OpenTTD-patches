from pathlib import Path
import subprocess, sys, re, shutil
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
BASE = ROOT / 'build-compiler-comparison'
for compiler in ('msvc', 'clang'):
    build = BASE / compiler
    driver = build / 'real-map-driver/null_v.cpp'
    text = driver.read_text(encoding='utf-8')
    old = '''#ifdef WITH_PASSENGER_BENCHMARK
\tif (!this->until_exit) {
\t\tIConsoleCmdExec("exec scripts/passenger_benchmark_end.scr 0");
\t}
#endif'''
    assert old in text
    text = text.replace(old, '''if (!this->until_exit && getenv("OPENTTD_PASSENGER_MEASURE") != nullptr) {
        IConsoleCmdExec("exec scripts/passenger_benchmark_end.scr 0");
    }''')
    driver.write_text(text, encoding='utf-8')
    # Fine grained elapsed counters remain elapsed; WPR supplies sampled CPU.
    for name in ('passenger_journey_active.cpp', 'passenger_service_database.cpp', 'passenger_journey_schedule.cpp'):
        path = build / 'cpu-measurement' / name
        text = path.read_text(encoding='utf-8').replace('MeasurementCpuClock', 'std::chrono::steady_clock')
        path.write_text(text, encoding='utf-8')
    subprocess.run([sys.executable, str(ROOT / 'utils/passenger_wpr_prepare.py'), str(build)], check=True)
    shutil.copy2(ROOT / 'docs/measurements/passenger-hyperpath-wpr-f7c44f0d-2026-10-17/cpu-sampled.wpaProfile', build / 'cpu-sampled.wpaProfile')
print('Prepared WPR gates and unmodified elapsed diagnostics')
