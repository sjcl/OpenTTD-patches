"""Copy the existing real-map runner, adding only paused probe commands."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[4]
WORK = ROOT / 'build-real-preparation-20261020'
original = ROOT / 'utils/passenger_precomputed_real_map.py'
text = original.read_text(encoding='utf-8')
text = text.replace('ROOT = pathlib.Path(__file__).resolve().parents[1]', f'ROOT = pathlib.Path({str(ROOT)!r})')
anchor = '    if (operation == "build_precomputed") {'
# The Python runner's script generation, not its C++ generator strings.
old = '            setup += "passenger_journey_stats build_precomputed\\n"'
new = '            setup += "passenger_journey_stats preparation_measure\\npassenger_journey_stats build_precomputed\\n"'
assert text.count(old) == 1
text = text.replace(old, new)
old = '        replay = "passenger_journey_stats wait_saved_replay\\n" if getattr(args, "wait_saved_replay", False) else ""'
new = '''        workers = int(os.environ.get("OPENTTD_REAL_WORKERS", "4"))
        replay = f"setting gui.passenger_precomputed_workers {workers}\\npassenger_journey_stats measurement_wait_background\\npassenger_journey_stats replay_measure\\n"
        if getattr(args, "wait_saved_replay", False): replay += "passenger_journey_stats wait_saved_replay\\n"
        if os.environ.get("OPENTTD_REAL_FIXTURE"):
            setup = "setting linkgraph.passenger_transfer_penalty 20\\n" + setup
            setup += "passenger_journey_stats mixed_replay_fixture\\n"
'''.rstrip()
assert text.count(old) == 1
text = text.replace(old, new)
target = WORK / 'real_runner.py'
target.write_text(text, encoding='utf-8', newline='\n')
metadata = dict(original=str(original), original_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),
    copy=str(target), copy_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
    changes=['Point Python imports to the repository utilities', 'Measure paused replay before any setting changes',
        'Set requested local worker count before replay', 'Measure initial preparation before existing warm-up verification',
        'Optional cost replacement fixture at an unchanged StateTicks'])
Path(__file__).with_name('runner-compat.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8', newline='\n')
