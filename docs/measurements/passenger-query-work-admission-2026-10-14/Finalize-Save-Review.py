"""Validate actual v4->v3 content conservation, merge eligibility and continuation."""
import argparse
import importlib.util
import json
import pathlib
import shutil

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('feature', HERE / 'Read-Save-Feature.py')
feature = importlib.util.module_from_spec(spec)
spec.loader.exec_module(feature)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=pathlib.Path)
p.add_argument('--archive', type=pathlib.Path, required=True)
a = p.parse_args()
build, output = a.build.resolve(), a.archive
output.mkdir(exist_ok=True)
names = ['v4-review-scanned-reader', 'v4-review-scanned-native', 'v3-review-scanned-reload', 'v3-review-scanned-continuation']
stages = {name: json.loads((build / name / 'stage.json').read_text(encoding='utf-8')) for name in names}
reader, native, reload, resumed = [stages[name] for name in names]
keys = ['packets', 'cargo_units', 'known_od_units', 'journey_packets', 'packet_state_digest', 'payment_digest', 'payments_valid']
checks = {key: reader['integrity'][key] == native['integrity'][key] == reload['integrity'][key] for key in keys}
tag = reader['integrity']
checks['v4_both_tag_values_present'] = tag['legacy_true_packets'] > 0 and tag['legacy_false_packets'] > 0
checks['packet_count_conserved'] = tag['legacy_true_packets'] + tag['legacy_false_packets'] == native['integrity']['packets']
checks['cargo_count_conserved'] = tag['legacy_true_units'] + tag['legacy_false_units'] == native['integrity']['cargo_units']
checks['opposite_history_mergeable'] = tag['opposite_history_mergeable_pairs'] > 0
checks['probe_and_native_save_sha256'] = reader['result_sha256'] == native['result_sha256']
checks['actual_256_ticks'] = resumed['actual_clock']['end_tick'] - resumed['actual_clock']['start_tick'] == 256
checks['no_error_pause_resumes'] = resumed['actual_clock']['error_resumes'] == 0
old = build / 'final-measured/q16-t49152/on-1/on-1-result.sav'
formats = {'old_input': feature.routing_feature(old)}
for name in names:
    formats[name] = feature.routing_feature(build / name / 'result.sav')
checks['actual_v4_to_v3'] = formats['old_input'] == 4 and all(v == 3 for key, v in formats.items() if key != 'old_input')
if not all(checks.values()):
    raise ValueError(checks)
for name in names:
    target = output / name
    target.mkdir(exist_ok=True)
    for filename in ['stage.json', 'save-integrity.json', 'on-1-console.txt', 'stderr.txt', 'continuation-driver.json']:
        source = build / name / filename
        if source.exists():
            shutil.copy2(source, target / filename)
excluded = ['v4-review-reader', 'v4-review-native', 'v3-review-reload', 'v4-review-old-roundtrip', 'v4-review-native-final',
    'v3-review-reload-final', 'v3-review-continuation', 'v4-review-old-reference', 'v4-review-old-continuation']
exclusion = {'reason': 'Incorrect -Q startup skipped NewGRF scanning. Missing GRFs caused identical cargo normalization and first-tick road-speed assertion in old and corrected binaries. No engine fix or assertion suppression was used; normal scan resolved both.', 'excluded_directories': excluded}
for name in ['v3-review-continuation', 'v4-review-old-continuation']:
    for crash in (build / name).glob('crash-*.log'):
        shutil.copy2(crash, output / (name + '-excluded-crash.log'))
result = {'stages': stages, 'actual_save_features': formats, 'checks': checks, 'excluded_setup_attempts': exclusion,
    'producer': 'Archived f519df10ab executable a1492347..., product sources identical to 11730ea682; git diff --quiet src excluding tests, cmake, CMakeLists returned 0.',
    'notes': ['The retired field exists only as a temporary scalar in an isolated reader; the native variant skips it as an unknown field.',
        'Packet, OD, Intent and payment fingerprints use explicit saved fields. Full probe/native v3 save hashes also agree.',
        'Runtime feature globals reset for saving; actual file versions above were read independently from SLXI.',
        'No Work-history field or merge restriction was added back to the product.']}
(output / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'checks': checks, 'actual_save_features': formats}, indent=2))
