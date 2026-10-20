"""Compare canonical state independently of timing/adoption status."""
from prepare import WORK, EVIDENCE, dump
import json

def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
checks = []
for kind, a, b in [
    ('initial/legacy/raw-reference', 'waiting/initial/A1/legacy', 'waiting/initial/B1/legacy'),
    ('initial/legacy', 'waiting/initial/A2/legacy', 'waiting/initial/B1/legacy'),
    ('initial/hyperpath', 'waiting/initial/A1/hyperpath', 'waiting/initial/B1/hyperpath'),
    ('initial/hyperpath/pilot', 'waiting/initial/A1/hyperpath', 'waiting/initial/pilot/hyperpath'),
    ('initial/hyperpath/workers8', 'waiting/initial/B1/hyperpath', 'waiting/initial/workers8/hyperpath'),
    ('replay/legacy', 'waiting/replay/A1/legacy', 'waiting/replay/B1/legacy'),
    ('replay/hyperpath', 'waiting/replay/A1/hyperpath', 'waiting/replay/B1/hyperpath'),
    ('replay/hyperpath/repeat-A', 'waiting/replay/A1/hyperpath', 'waiting/replay/A2/hyperpath'),
    ('replay/hyperpath/repeat-B', 'waiting/replay/B1/hyperpath', 'waiting/replay/B2/hyperpath'),
    ('replay/hyperpath/workers8', 'waiting/replay/B1/hyperpath', 'waiting/replay/workers8/hyperpath'),
    ('multiplayer/input-unpause', 'fixtures/hyperpath', 'fixtures-mp/hyperpath'),
    ('multiplayer/host-cross-version', 'waiting/multiplayer-clean/baseline/server', 'waiting/multiplayer-clean/candidate/server'),
    ('multiplayer/client-cross-version', 'waiting/multiplayer-clean/baseline/client', 'waiting/multiplayer-clean/candidate/client')]:
    left, right = WORK / a / 'save-integrity.json', WORK / b / 'save-integrity.json'
    if not left.exists() or not right.exists(): continue
    av, bv = read(left), read(right)
    differences = {k: [av.get(k), bv.get(k)] for k in sorted(av.keys() | bv.keys()) if av.get(k) != bv.get(k)}
    checks.append({'case': kind, 'equal': not differences, 'fields': len(av), 'differences': differences,
        'left': a, 'right': b})
dump(EVIDENCE / 'canonical-comparisons.json', checks)
for check in checks: print(check['case'], 'equal' if check['equal'] else check['differences'], check['fields'], flush=True)
assert all(check['equal'] for check in checks)
