"""Narrow the warm-up instrumentation anchor after a second timer was added upstream."""
from pathlib import Path
import hashlib
import json
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
original=ROOT/'utils/passenger_estimated_prepare.py'
source=original.read_text(encoding='utf-8')
before="    anchor = '        const auto started = std::chrono::steady_clock::now();'"
after="    anchor = '        bool ready = false;\\n        const auto started = std::chrono::steady_clock::now();'"
assert source.count(before)==1
result=source.replace(before,after)
target=WORK/'estimated_prepare_compat.py';target.write_text(result,encoding='utf-8')
(WORK/'prepare-compat.json').write_text(json.dumps(dict(original=str(original),original_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),
    measurement_copy=str(target),copy_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),before=before,after=after,
    reason='wait_saved_replay introduced a second timer. Select only build_precomputed warm-up, preserving the same inserted code.',
    product_sources_unchanged=True),indent=2)+'\n')
