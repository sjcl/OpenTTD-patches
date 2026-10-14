"""Rehash final saves and compare logical work, decisions, transport and admission."""
import argparse
import importlib.util
import json
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('summary', HERE / 'Summarize.py')
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('warm', type=pathlib.Path)
p.add_argument('cold', type=pathlib.Path)
p.add_argument('--output', type=pathlib.Path, required=True)
a = p.parse_args()
wm, wr = summary.profile.read_manifest(a.warm)
cm, cr = summary.profile.read_manifest(a.cold)
w, c = wr[0], cr[0]
for run in (w, c):
    if not run['valid']:
        raise ValueError(run['errors'])
    summary.check_accounting(run)
keys = ['Periodic admission', 'Periodic allocation', 'Periodic Work outcomes', 'Periodic missing',
        'Work exhausted OD transport', 'Waiting age sample']
details = {key: w['diagnostics']['records'].get(key) == c['diagnostics']['records'].get(key) for key in keys}
result = {'same_binary': wm['binary_sha256'] == cm['binary_sha256'],
          'same_input': wm['package'] == cm['package'],
          'same_semantics': w['semantic_signature'] == c['semantic_signature'],
          'same_admission': all(details.values()), 'checks': details,
          'same_full_save': w['final_save_sha256'] == c['final_save_sha256'],
          'full_save_sha256': w['final_save_sha256'],
          'warm_mean_ms': w['derived']['simulation_mean_ms'], 'cold_mean_ms': c['derived']['simulation_mean_ms']}
a.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
if not all(result[k] for k in ['same_binary', 'same_input', 'same_semantics', 'same_admission', 'same_full_save']):
    raise ValueError('Cache invariance failed: ' + str(result))
print(json.dumps(result, indent=2))
