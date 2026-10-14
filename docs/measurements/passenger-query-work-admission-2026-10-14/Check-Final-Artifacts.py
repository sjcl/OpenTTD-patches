"""Check JSON and lossless stderr archives before committing final evidence."""
import gzip
import hashlib
import json
import pathlib

root = pathlib.Path(__file__).resolve().parent
count = 0
for path in root.rglob('*.json'):
    json.loads(path.read_text(encoding='utf-8-sig'))
    count += 1
summary = json.loads((root / 'final-review/summary.json').read_text())
for run in summary['runs']:
    entry = run['archived_stderr']
    data = gzip.decompress((root / 'final-review' / f"{run['balanced_sequence_index']:02}-{run['condition']}" / entry['file']).read_bytes())
    assert len(data) == entry['bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256']
for path in (root / 'final-network-review').glob('*/archive.json'):
    entry = json.loads(path.read_text())
    for role, value in entry['stderr'].items():
        data = gzip.decompress((path.parent / role / value['complete_gzip']).read_bytes())
        assert len(data) == value['bytes'] and hashlib.sha256(data).hexdigest() == value['sha256']
assert all(summary['cache_determinism'].values())
assert all(json.loads((root / 'final-save-review/result.json').read_text())['checks'].values())
print(f'Valid JSON: {count}; complete performance/network stderr gzip hashes match; cache determinism and real save checks PASS')
