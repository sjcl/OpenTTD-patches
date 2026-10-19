from pathlib import Path
from collections import defaultdict
from decimal import Decimal
import csv, json, re, statistics
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
BASE = ROOT / 'build-compiler-comparison'
names = ['wpr-msvc-1', 'wpr-clang-1', 'wpr-clang-2', 'wpr-msvc-2']
route_roots = (
    'PassengerServiceDatabase::PlanPrecomputed', 'PassengerServiceDatabase::PlanInternal',
    'PassengerPrecomputedTable::Plan', 'PassengerJourneyPlanner::Plan',
    'PassengerJourneyPlanner::PlanOnboard', 'PassengerSchedulePlanner::Plan',
)
boarding_roots = ('PassengerJourneyLoadingContext::CanBoard', 'PassengerJourneyLoadingContext::MayUseHyperpathPolicy')
rows = []
canonical = {}
def ms(value): return float(Decimal(value.replace(',', '')))
for name in names:
    directory = BASE / 'runs' / name
    metadata = json.loads((directory / 'process.json').read_text())
    clock = metadata['actual-ticks.json']
    assert metadata['simulation_timing_valid'] and metadata['warm_complete']
    assert len(clock['samples']) == clock['end_tick'] - clock['start_tick'] == 1024
    pid, tid = map(int, (directory / 'wpr-ready.txt').read_text().split())
    sums = defaultdict(float)
    counts = defaultdict(int)
    functions = defaultdict(float)
    csv_path = directory / 'wpa/CPU_Usage_(Sampled)_Stacks.csv'
    with csv_path.open(encoding='utf-8-sig', newline='') as f:
        for row in csv.DictReader(f):
            if not row['Thread ID'] or not row['Stack']: continue
            assert row['Process'] == f'openttd.exe ({pid})'
            weight = ms(row['Weight (ms)'])
            samples = int(row['Count'].replace(',', ''))
            sums['all_threads_ms'] += weight
            if int(row['Thread ID']) != tid: continue
            sums['main_thread_ms'] += weight
            frames = re.split(r'/(?=[^/!]+\.(?:exe|dll|sys)!)', row['Stack'], flags=re.I)
            frames = [re.sub(r'\+0x[0-9a-fA-F]+$', '', x) for x in frames]
            world = 'openttd.exe!StateGameLoop' in frames
            if not world: continue
            product = [x.partition('!')[2] for x in frames if x.startswith('openttd.exe!')]
            route = any(any(x.startswith(root) for root in route_roots) for x in product)
            boarding = any(any(x.startswith(root) for root in boarding_roots) for x in product)
            passenger = any(x.startswith('Passenger') or x.startswith(('RunPassengerWaitingEvents', 'PreparePassengerJourneyLoading', 'ProcessPassenger', 'BeginPassengerJourneyTick')) for x in product)
            for key, include in [('world_ms', True), ('passenger_ms', passenger), ('route_ms', route), ('boarding_ms', boarding), ('route_or_boarding_ms', route or boarding)]:
                if include:
                    sums[key] += weight
                    counts[key] += samples
            for fn in set(product): functions[fn] += weight
    console = (directory / 'on-1-console.txt').read_text(encoding='utf-8', errors='replace')
    warm = re.search(r'Precomputed warm: ready=true; elapsed_us=(\d+); cpu_us=(\d+)', console)
    assert warm
    clock_cpu_ms = sum(s[1] for s in clock['samples']) / 1000
    row = dict(name=name, compiler=name.split('-')[1], ticks=1024, start_tick=clock['start_tick'], end_tick=clock['end_tick'], sampled_cpu_ms=dict(sums), sampled_counts=dict(counts), per_tick_cpu_ms={k:v/1024 for k,v in sums.items()}, world_thread_counter_ms=clock_cpu_ms, build_wall_seconds=int(warm[1])/1e6, build_cpu_seconds=int(warm[2])/1e6, warning_lines=metadata['newgrf_warning_lines'], error_resumes=clock['error_resumes'], initial_error_resumes=metadata['script_error_pause_resumes'])
    rows.append(row)
    canonical[name] = metadata['save-integrity.json']
    with (directory / 'function-cpu.csv').open('w', encoding='utf-8', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['function', 'inclusive_cpu_ms', 'cpu_ms_per_tick'])
        for fn, value in sorted(functions.items(), key=lambda item:-item[1]): writer.writerow([fn, value, value/1024])
reference = canonical[names[0]]
mismatches = {name: {key: [reference.get(key), data.get(key)] for key in set(reference)|set(data) if reference.get(key) != data.get(key)} for name, data in canonical.items()}
summary = {}
for compiler in ('msvc', 'clang'):
    selected = [r for r in rows if r['compiler'] == compiler]
    summary[compiler] = {key: statistics.median(r[key] for r in selected) for key in ('build_wall_seconds', 'build_cpu_seconds', 'world_thread_counter_ms')}
    summary[compiler]['cpu_ms_per_tick'] = {key:statistics.median(r['per_tick_cpu_ms'].get(key,0) for r in selected) for key in rows[0]['per_tick_cpu_ms']}
output = dict(runs=rows, medians=summary, canonical=canonical, mismatches=mismatches, route_roots=route_roots, boarding_roots=boarding_roots)
(BASE/'summary.json').write_text(json.dumps(output, indent=2, ensure_ascii=False))
print(json.dumps(dict(medians=summary, mismatches=mismatches), indent=2))
