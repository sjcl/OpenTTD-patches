"""Split one function's main-thread samples by caller and by callee from the saved WPA stacks.

Each row of the exported stacks CSV is one complete stack with its own (leaf) weight. Only stacks
containing StateGameLoop are counted, and the target frame is taken at its outermost occurrence.

Usage: python -I frame_neighbours.py MEASUREMENT_DIR FUNCTION OUTPUT_JSON
"""

import collections
import csv
import gzip
import json
import sys

TICKS = 2048
ROUNDS = {'A': ('01-A1', '04-A2'), 'B': ('02-B1', '03-B2')}


def split(path, target):
	callers, callees = collections.Counter(), collections.Counter()
	with gzip.open(path, 'rt', encoding='utf-8', errors='replace') as f:
		for row in csv.DictReader(f):
			stack = row['Stack']
			if not stack or not row['Thread ID'] or 'StateGameLoop' not in stack: continue
			frames = stack.split('/')
			index = next((i for i, frame in enumerate(frames) if frame.endswith('!' + target)), None)
			if index is None: continue
			weight = float(row['Weight (ms)'].replace(',', ''))
			callers['<total>'] += weight
			callers[frames[index - 1]] += weight
			callees[frames[index + 1] if index + 1 < len(frames) else '<self>'] += weight
	return callers, callees


def main():
	root, target, output = sys.argv[1:4]
	result = {'function': target, 'ticks': TICKS, 'unit': 'ms/tick', 'modes': {}}
	for mode in ('legacy', 'hyperpath'):
		runs = {}
		for version, rounds in ROUNDS.items():
			for run in rounds:
				runs[run] = split(f'{root}/{run}/{mode}/CPU_Usage_(Sampled)_Stacks.csv.gz', target)
		table = {}
		for side, index in (('callers', 0), ('callees', 1)):
			names = set().union(*(runs[run][index] for run in runs))
			rows = {}
			for name in names:
				entry = {run: runs[run][index][name] / TICKS for run in runs}
				for version, rounds in ROUNDS.items():
					entry[version] = sum(entry[run] for run in rounds) / len(rounds)
				entry['delta'] = entry['B'] - entry['A']
				rows[name] = entry
			table[side] = dict(sorted(rows.items(), key=lambda item: -abs(item[1]['delta'])))
		result['modes'][mode] = table
	with open(output, 'w', encoding='utf-8', newline='\n') as f:
		json.dump(result, f, indent=1)
		f.write('\n')


if __name__ == '__main__':
	main()
