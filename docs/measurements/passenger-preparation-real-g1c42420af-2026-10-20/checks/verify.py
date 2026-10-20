"""Verify source pinning, recoverability, and staged Git bytes before publishing."""
from prepare import ROOT, WORK, EVIDENCE, BASE, NEW, dump
from pathlib import Path
import argparse
import gzip
import hashlib
import json
import subprocess

REPORT = ROOT / 'docs/passenger-waiting-real-g1c42420af-2026-10-20.md'
RUNBOOK = ROOT / 'docs/passenger-wpr-runbook.md'
ATTRIBUTES = ROOT / 'docs/.gitattributes'

def sha(p):
    with p.open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()
def read(p): return json.loads(p.read_text(encoding='utf-8-sig'))
def git(*args): return subprocess.check_output(['git', *args], cwd=ROOT)

def normalize():
    paths = [ATTRIBUTES, REPORT, RUNBOOK] + [p for p in EVIDENCE.rglob('*') if p.is_file() and p.suffix not in ['.gz', '.wprp']]
    for p in paths: p.write_text(p.read_text(encoding='utf-8-sig'), encoding='utf-8', newline='\n')

def manifest():
    subprocess.run(['git', 'diff', '--quiet', NEW, '--', 'src'], cwd=ROOT, check=True)
    source = read(EVIDENCE / 'source-identity.json')
    assert source['candidate'] == NEW and source['baseline'] == BASE
    assert git('rev-parse', NEW + ':src').decode().strip() == source['candidate_src_tree']
    assert git('rev-parse', BASE + ':src').decode().strip() == source['baseline_src_tree']
    backup = read(EVIDENCE / 'backup-manifest.json')
    base = Path(backup['path'])
    for name, expected in backup['files'].items():
        p = base / name
        assert p.stat().st_size == expected['bytes'] and sha(p) == expected['sha256'], name
    for label in ['baseline', 'candidate']:
        identity = read(EVIDENCE / f'{label}-waiting-instrumentation.json')
        for name, expected in identity['files'].items():
            p = EVIDENCE / 'build' / label / (name + '.gz')
            with gzip.open(p, 'rb') as f: assert hashlib.file_digest(f, 'sha256').hexdigest() == expected, name
    adopted = read(EVIDENCE / 'validation.json')['accepted_waiting_runs']
    assert len(adopted) == 13
    for name in adopted:
        folder = EVIDENCE / 'runs' / Path(name)
        native, observed, canonical = [read(folder / n) for n in ['waiting-ready.json', 'observed.json', 'save-integrity.json']]
        assert native['ready'] and native['tick_before'] == native['tick_after'] == canonical['state_ticks'] == 3201034745
        assert not observed['contamination']
        label = 'baseline' if '/A' in name.replace('\\', '/') or '/baseline/' in name.replace('\\', '/') else 'candidate'
        expected = backup['files'][f'binaries/{label}/Release/openttd.exe']['sha256']
        assert observed['binary_sha256'] == expected, name
    paths = [p for p in EVIDENCE.rglob('*') if p.is_file() and p.name not in ['evidence-sha256.json', 'git-verification.json']]
    paths += [REPORT, RUNBOOK, ATTRIBUTES]
    dump(EVIDENCE / 'evidence-sha256.json', {p.relative_to(ROOT).as_posix(): {'bytes': p.stat().st_size, 'sha256': sha(p)} for p in sorted(paths)})
    print(f'Verified independent backup, generated TUs, source trees and {len(adopted)} adopted runs', flush=True)

def staged():
    expected = read(EVIDENCE / 'evidence-sha256.json')
    manifest_name = (EVIDENCE / 'evidence-sha256.json').relative_to(ROOT).as_posix()
    manifest_bytes = git('show', ':' + manifest_name)
    assert manifest_bytes == (EVIDENCE / 'evidence-sha256.json').read_bytes()
    for name, info in expected.items():
        data = git('show', ':' + name)
        assert len(data) == info['bytes'] and hashlib.sha256(data).hexdigest() == info['sha256'], name
    subprocess.run(['git', 'diff', '--cached', '--check'], cwd=ROOT, check=True)
    staged_names = git('diff', '--cached', '--name-only').decode().splitlines()
    prefix = EVIDENCE.relative_to(ROOT).as_posix() + '/'
    assert all(n.startswith(prefix) or n in [REPORT.relative_to(ROOT).as_posix(), RUNBOOK.relative_to(ROOT).as_posix(), ATTRIBUTES.relative_to(ROOT).as_posix()] for n in staged_names)
    assert all(Path(n).suffix not in ['.sav', '.exe', '.pdb', '.etl'] for n in staged_names)
    dump(EVIDENCE / 'git-verification.json', {'staged_blob_hashes_verified': len(expected), 'git_diff_cached_check': True,
        'manifest_blob_sha256': hashlib.sha256(manifest_bytes).hexdigest(),
        'scope': 'Report, runbook, LF attributes and small evidence only', 'product_source_unchanged_from': NEW})
    print(f'Verified {len(expected)} staged Git blob hashes', flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('phase', choices=['normalize', 'manifest', 'staged'])
    args = parser.parse_args()
    globals()[args.phase]()
