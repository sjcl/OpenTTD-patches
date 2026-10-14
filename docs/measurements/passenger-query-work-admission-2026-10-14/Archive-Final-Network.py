"""Archive loopback evidence, retaining complete compressed stderr and close logs."""
import argparse
import gzip
import hashlib
import json
import pathlib
import shutil

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('run', type=pathlib.Path)
p.add_argument('archive', type=pathlib.Path)
p.add_argument('--outcome', choices=['pass', 'fail'], required=True)
p.add_argument('--run-log', type=pathlib.Path, required=True)
a = p.parse_args()
root, output = a.run.resolve(), a.archive.resolve()
output.mkdir(parents=True, exist_ok=True)
metadata = {'outcome': a.outcome, 'local_run': str(root), 'stderr': {}}
for role in ['server', 'client-initial', 'client-late']:
    target = output / role
    target.mkdir(exist_ok=True)
    for name in ['console.txt', 'progress-console.txt', 'stdout.txt', 'save-integrity.json']:
        source = root / role / name
        if source.exists():
            shutil.copy2(source, target / name)
    raw = (root / role / 'stderr.txt').read_bytes()
    (target / 'stderr.txt.gz').write_bytes(gzip.compress(raw, mtime=0))
    lines = raw.decode('utf-8', errors='replace').splitlines()
    events = [line for line in lines if '[net:' in line or any(term in line.lower() for term in
              ['sync_err:', 'sync error', 'assertion', 'desync detected', 'final close probe', 'final error probe'])]
    (target / 'network-events.txt').write_text('\n'.join(events) + '\n', encoding='utf-8')
    metadata['stderr'][role] = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(),
        'complete_gzip': 'stderr.txt.gz', 'newgrf_capacity_warning_count': sum('It changed vehicle capacity' in line for line in lines)}
shutil.copy2(a.run_log, output / 'runner.log')
if (root / 'result.json').exists():
    result = json.loads((root / 'result.json').read_text(encoding='utf-8'))
    if a.outcome == 'pass' and (not result['cargo_state_equal'] or result['detected_sync_failure'] or result['connected_clients_at_end'] != 2):
        raise ValueError('Cannot archive failed network evidence as passing')
    shutil.copy2(root / 'result.json', output / 'result.json')
elif a.outcome == 'pass':
    raise ValueError('Passing result is missing')
(output / 'archive.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
print(json.dumps(metadata, indent=2))
