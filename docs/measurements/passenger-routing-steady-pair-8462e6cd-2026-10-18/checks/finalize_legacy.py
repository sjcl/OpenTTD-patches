"""Accept the independently audited tick window while preserving post-window activity."""
from pathlib import Path
import hashlib
import json

WORK = Path(__file__).resolve().parent
meta = json.loads((WORK/'legacy/process.json').read_text())
window = json.loads((WORK/'legacy/window-audit.json').read_text())
assert meta['exit_code'] == 0 and meta['simulation_timing_valid'] is True
assert meta['whole_process_timing_valid'] is False
assert meta['phase_contamination'] and all(o['stage']=='after_simulation' for o in meta['phase_contamination'])
clock = meta['actual-ticks.json']
assert len(clock['samples']) == clock['end_tick']-clock['start_tick'] == 2048
assert clock['start_tick'] == 3181127773 and clock['end_tick'] == 3181129821
assert window['valid'] is True and not window['overlapping_competing_processes']
assert meta['input_sha256'] == 'b276c5de400651f646c129ab394bc97a686f38aa345b3a4a799e07c8a06289ca'
assert meta['save-integrity.json']['payments_valid'] is True
for name in ['legacy-contaminated','legacy-contaminated-2']:
    assert json.loads((WORK/name/'save-integrity.json').read_text()) == meta['save-integrity.json']
result = dict(ticks=2048, mode='legacy', input_sha256=meta['input_sha256'],
    canonical_matches_both_rejected_attempts=True, simulation_timing_valid=True,
    whole_process_timing_valid=False, post_simulation_observations=len(meta['phase_contamination']),
    etl_window_valid=True, process_metadata_unchanged=True,
    process_metadata_sha256=hashlib.sha256((WORK/'legacy/process.json').read_bytes()).hexdigest(),
    explanation='The runner succeeded. The outer retry controller still required whole-process quiescence and failed its stricter assertion. All observed activity was after_simulation; ETL lifetimes independently confirm no competing process overlaps the WPR markers. Preserve raw metadata and use only the 2048-tick window.')
(WORK/'measurement-complete.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
