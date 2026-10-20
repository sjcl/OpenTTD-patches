"""Normalise per-cause routing timers by the expiry/failure timers, whose packet counts are identical in A and B.

Usage: python -I cause_normalise.py <measurement dir>
"""
import gzip, json, re, sys
from pathlib import Path

root = Path(sys.argv[1])
RUNS = ['01-A1', '02-B1', '03-B2', '04-A2']
pattern = re.compile(r'Passenger scheduler cause: cause=(\w+); packets=(\d+); microseconds=(\d+)')
out = {}
for mode in ['legacy', 'hyperpath']:
	base = None
	for run in RUNS:
		with gzip.open(root / run / mode / 'on-1-console.txt.gz', 'rt', encoding='utf-8', errors='replace') as f:
			causes = {m[1]: (int(m[2]), int(m[3])) for m in pattern.finditer(f.read())}
		reference = causes['expiry'][1] + causes['failure'][1]
		if base is None: base = reference
		scale = reference / base
		out[f'{run}/{mode}'] = {
			'scale_vs_01-A1': scale,
			'causes': {c: {'packets': p, 'us': us, 'us_normalised': us / scale} for c, (p, us) in causes.items()},
			'new_plus_generation_packets': causes['new'][0] + causes['generation'][0],
			'new_plus_generation_us_normalised': (causes['new'][1] + causes['generation'][1]) / scale,
		}
print(json.dumps(out, indent=1))
