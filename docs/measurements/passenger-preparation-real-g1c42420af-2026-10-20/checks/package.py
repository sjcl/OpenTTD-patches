"""Preserve inputs/binaries outside the worktree, then package small evidence."""
from prepare import ROOT, WORK, EVIDENCE, BASE, NEW, dump
from pathlib import Path
import argparse
import gzip
import hashlib
import json
import re
import shutil
import subprocess
import zipfile

BACKUP = Path('C:/Users/bebe/OpenTTD-measurements/passenger-waiting-real-20261020-fe663cfa')
REPORT = ROOT / 'docs/passenger-waiting-real-g1c42420af-2026-10-20.md'

def read(p): return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()
def identity(p): return {'bytes': p.stat().st_size, 'sha256': sha(p)}
def text_copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(source.read_text(encoding='utf-8-sig', errors='replace').replace('\r\n', '\n').rstrip() + '\n', encoding='utf-8', newline='\n')
def compress(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    with source.open('rb') as src, target.open('wb') as dst:
        with gzip.GzipFile(filename='', fileobj=dst, mode='wb', mtime=0) as stream: shutil.copyfileobj(src, stream)
    with gzip.open(target, 'rb') as stream:
        assert hashlib.file_digest(stream, 'sha256').hexdigest() == sha(source)

def backup():
    BACKUP.mkdir(parents=True, exist_ok=False)
    for label in ['baseline', 'candidate']:
        shutil.copytree(WORK / 'artifacts-waiting' / label, BACKUP / 'binaries' / label)
    target = BACKUP / 'fixture-generator'
    target.mkdir()
    for name in ['Release/openttd.exe', 'Release/openttd.pdb', 'lang/english.lng', 'build-identity.json']:
        p = target / name; p.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(WORK / 'artifacts-v2/candidate' / name, p)
    with zipfile.ZipFile(BACKUP / 'frozen-input.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for p in sorted((WORK / 'input').rglob('*')):
            if p.is_file(): archive.write(p, p.relative_to(WORK / 'input').as_posix())
    fixtures = BACKUP / 'inputs'; fixtures.mkdir()
    for src, name in [('fixtures/legacy/result.sav', 'retained-legacy.sav'),
                      ('fixtures/hyperpath/result.sav', 'retained-hyperpath.sav'),
                      ('fixtures-mp/hyperpath/result.sav', 'retained-hyperpath-mp-unpaused.sav')]:
        shutil.copy2(WORK / src, fixtures / name)
    traces = BACKUP / 'traces'; traces.mkdir()
    for src, name in [('pilot/cpu.etl', 'pilot.etl'), ('runs/A-baseline/legacy/cpu.etl', 'excluded-normal-legacy.etl'),
                      ('runs/A-baseline/hyperpath/cpu.etl', 'excluded-normal-hyperpath.etl')]:
        shutil.copy2(WORK / src, traces / name)
    source_save = Path('C:/Users/bebe/OneDrive/ドキュメント/OpenTTD/save/g1c42420af.sav')
    expected = read(WORK / 'input/package.json')['input_sha256']
    assert sha(source_save) == sha(WORK / 'input/input.sav') == expected
    with zipfile.ZipFile(BACKUP / 'frozen-input.zip') as archive:
        assert hashlib.sha256(archive.read('input.sav')).hexdigest() == expected
        manifest = read(WORK / 'input/package.json')
        for row in manifest['assets']:
            assert hashlib.sha256(archive.read(row['path'])).hexdigest() == row['sha256']
    files = {p.relative_to(BACKUP).as_posix(): identity(p) for p in sorted(BACKUP.rglob('*')) if p.is_file()}
    for p in BACKUP.rglob('*'):
        if p.is_file(): p.chmod(0o444)
    dump(EVIDENCE / 'backup-manifest.json', {'path': str(BACKUP), 'files': files, 'independent_copies': True,
        'original_save_unchanged': True, 'input_sha256': expected,
        'trace_use': 'Only the pilot is method evidence. Both normal-tick ETLs are retained but excluded after the user narrowed scope.'})

def evidence():
    input_out = EVIDENCE / 'input'; input_out.mkdir(exist_ok=True)
    for name in ['package.json', 'openttd.cfg']: text_copy(WORK / 'input' / name, input_out / name)
    dump(input_out / 'identity.json', {'source': 'C:/Users/bebe/OneDrive/ドキュメント/OpenTTD/save/g1c42420af.sav',
        **identity(WORK / 'input/input.sav'), 'tick': 3201034745,
        'fixtures': {name: identity(WORK / path / 'result.sav') for name, path in [
            ('legacy', 'fixtures/legacy'), ('hyperpath', 'fixtures/hyperpath'), ('hyperpath_mp', 'fixtures-mp/hyperpath')]}})
    for p in WORK.glob('*.log'): compress(p, EVIDENCE / 'logs' / (p.name + '.gz'))
    for name in ['profile-interval.txt', 'wpr-status-before.txt', 'normal-tick-excluded.json']:
        text_copy(WORK / name, EVIDENCE / 'checks' / name)
    text_copy(WORK / 'host.json', EVIDENCE / 'host.json')
    shutil.copy2(WORK / 'cpu.wprp', EVIDENCE / 'cpu.wprp')
    for name in ['trace-stats.txt', 'recorder-markers.txt']:
        text_copy(WORK / 'pilot' / name, EVIDENCE / 'pilot' / name)
    for label, build in [('baseline', WORK / 'baseline-build'), ('candidate', ROOT / 'build-preparation')]:
        compress(build / 'CMakeCache.txt', EVIDENCE / 'build' / label / 'CMakeCache.txt.gz')
        for p in (build / 'openttd_lib.dir/Release/openttd_lib.tlog').glob('clang-cl.command.*.tlog'):
            compress(p, EVIDENCE / 'build' / label / (p.name + '.gz'))
        for p in WORK.glob(label + '-*.command.json'):
            text_copy(p, EVIDENCE / 'build' / label / p.name)
        for folder in ['save-integrity', 'real-map-driver', 'waiting-probe']:
            for p in sorted((build / folder).glob('*')):
                if p.is_file(): compress(p, EVIDENCE / 'build' / label / folder / (p.name + '.gz'))
    text_copy(WORK / 'estimated_prepare_compat.py', EVIDENCE / 'checks/estimated_prepare_compat.py')
    # Only tiny witness files and compressed raw logs, never assets/save/exe/PDB.
    for base in ['waiting', 'fixtures', 'fixtures-mp', 'preflight-hyperpath', 'runs']:
        folder = WORK / base
        for p in sorted(folder.rglob('*')):
            if not p.is_file(): continue
            if any(part in PROFILE_ASSETS for part in p.relative_to(folder).parts[:-1]): continue
            rel = p.relative_to(WORK)
            if p.name in ['waiting-ready.json', 'waiting-armed.json', 'observed.json', 'live-observer.json', 'save-integrity.json',
                          'adoption.json', 'canonical-comparison.json', 'launch.json', 'process.json', 'preparation-probes.json',
                          'window-audit.json', 'actual-ticks.json'] or p.suffix == '.scr' or p.name == 'openttd.cfg':
                text_copy(p, EVIDENCE / 'runs' / rel)
            elif p.name in ['stderr.txt', 'stdout.txt', 'on-1-console.txt']:
                compress(p, EVIDENCE / 'runs' / rel.with_suffix(rel.suffix + '.gz'))
    stats = (WORK / 'pilot/trace-stats.txt').read_text(encoding='utf-8-sig')
    assert re.search(r'Total # Lost Events\s+: 0\b', stats) and re.search(r'Total # Lost Buffers\s+: 0\b', stats)
    adopted = []
    for p in sorted((WORK / 'waiting').rglob('waiting-ready.json')):
        native = read(p); directory = p.parent
        assert native['tick_before'] == native['tick_after'] == 3201034745
        if not (directory / 'observed.json').exists(): continue
        observation = read(directory / 'observed.json')
        assert not observation['contamination'] and observation.get('census_count', 1) > 0
        canonical = read(directory / 'save-integrity.json')
        assert canonical['payments_valid'] and canonical['state_ticks'] == 3201034745
        adopted.append(str(directory.relative_to(WORK)))
    checks = read(EVIDENCE / 'canonical-comparisons.json')
    assert len(checks) >= 11 and all(c['equal'] and c['fields'] == 24 for c in checks)
    native = {}
    for label, cases in [('baseline', 801), ('candidate', 805)]:
        log = (WORK / f'{label}-waiting-native.log').read_text(encoding='utf-8')
        match = re.search(r'Passed all (\d+) test cases with (\d+) assertions', log)
        assert match and int(match[1]) == cases
        native[label] = {'cases': int(match[1]), 'assertions': int(match[2]), 'passed': True}
    dump(EVIDENCE / 'validation.json', {'accepted_waiting_runs': adopted, 'accepted_count': len(adopted),
        'tick_unchanged': True, 'canonical_all_24_fields_equal': True, 'native': native,
        'hyperpath_filter_validate': 'Not run: normal simulation ticks are outside the user-requested scope. Canonical equality is reported independently.',
        'normal_tick_performance': 'Excluded', 'gui': 'Real Win32 GDI, threaded, 640x480, hidden',
        'multiplayer': 'Real encrypted TCP loopback, spectator, stopped at first successful sync; paused host and client StateTicks unchanged'})

def excluded_pair():
    target = BACKUP / 'excluded-normal-baseline'
    target.mkdir(exist_ok=False)
    for name in ['Release/openttd.exe', 'Release/openttd.pdb', 'lang/english.lng', 'build-identity.json']:
        p = target / name; p.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(WORK / 'artifacts-v2/baseline' / name, p)
        p.chmod(0o444)
    manifest = read(EVIDENCE / 'backup-manifest.json')
    for p in target.rglob('*'):
        if p.is_file(): manifest['files'][p.relative_to(BACKUP).as_posix()] = identity(p)
    manifest['trace_binary_pairing'] = {
        'excluded-normal-legacy.etl': 'excluded-normal-baseline/Release/openttd.exe + openttd.pdb',
        'excluded-normal-hyperpath.etl': 'excluded-normal-baseline/Release/openttd.exe + openttd.pdb',
        'waiting': 'binaries/baseline and binaries/candidate; waiting metrics do not use ETLs'}
    dump(EVIDENCE / 'backup-manifest.json', manifest)

PROFILE_ASSETS = {'baseset', 'newgrf', 'content_download', 'ai', 'game', 'lang', 'Release'}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('phase', choices=['backup', 'evidence', 'excluded_pair'])
    args = parser.parse_args()
    globals()[args.phase]()
