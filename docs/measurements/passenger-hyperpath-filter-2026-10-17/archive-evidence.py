"""Freeze text evidence, source identities and copied measurement TUs; omit assets/saves/binaries."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def dump(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('package', type=Path)
    args = parser.parse_args()
    output = Path(__file__).resolve().parent
    root = output.parents[2]

    def git(*arguments):
        return subprocess.check_output(['git', '-C', str(root), *arguments], text=True).strip()

    if git('diff', 'HEAD', '--', 'src'):
        raise ValueError('Product sources changed')

    def copy(source, target, compress=False):
        target.parent.mkdir(parents=True, exist_ok=True)
        if compress:
            target.write_bytes(gzip.compress(source.read_bytes(), mtime=0))
        else:
            shutil.copyfile(source, target)

    for name in ('off-a', 'on-a', 'on-b', 'off-b'):
        folder = args.build / 'runs' / name
        process = json.loads((folder / 'process.json').read_text(encoding='utf-8'))
        if process['binary_sha256'] != sha(args.build / 'Release/openttd.exe'):
            raise ValueError('Measurement binary changed: ' + name)
        for filename in ('process.json', 'actual-ticks.json', 'save-integrity.json',
                         'midpoint-save-integrity.json', 'rebuild-transition.json'):
            copy(folder / filename, output / 'runs' / name / filename)
        for filename in ('stdout.txt', 'stderr.txt', 'on-1-console.txt'):
            copy(folder / filename, output / 'runs' / name / (filename + '.gz'), True)
        for script in (folder / 'scripts').glob('*.scr'):
            copy(script, output / 'runs' / name / 'scripts' / script.name)
    copied = {}
    for subfolder in ('save-integrity', 'real-map-driver'):
        for path in sorted((args.build / subfolder).rglob('*')):
            if path.is_file() and path.suffix in ('.h', '.cpp'):
                relative = path.relative_to(args.build).as_posix()
                copied[relative] = sha(path)
                copy(path, output / 'copied-tus' / (relative + '.gz'), True)
    for name in ('native-hyperpath.log', 'synthetic-filter.log', 'real-map-instrumentation.json'):
        copy(args.build / name, output / 'checks' / name)
    for name in ('build.log', 'build-integrity.log'):
        copy(args.build / name, output / 'checks' / (name + '.gz'), True)
    package = json.loads((args.package / 'package.json').read_text(encoding='utf-8'))
    if sha(args.package / 'input.sav') != package['input_sha256']:
        raise ValueError('Input differs from frozen package')
    for asset in package['assets']:
        if sha(args.package / asset['path']) != asset['sha256']:
            raise ValueError('Frozen asset changed: ' + asset['path'])
    for name in ('off-a', 'on-a', 'on-b', 'off-b'):
        process = json.loads((args.build / 'runs' / name / 'process.json').read_text(encoding='utf-8'))
        if process['input_sha256'] != package['input_sha256']:
            raise ValueError('Run input differs from frozen package: ' + name)
    copy(args.package / 'package.json', output / 'package.json')
    copy(args.package / 'openttd.cfg', output / 'openttd.cfg')
    dump(output / 'source-identity.json', dict(
        commit=git('rev-parse', 'HEAD'), product_src_tree=git('rev-parse', 'HEAD:src'),
        product_sources_unchanged=True, binary_sha256=sha(args.build / 'Release/openttd.exe'),
        test_binary_sha256=sha(args.build / 'Release/openttd_test.exe'),
        runner_sha256=sha(root / 'utils/passenger_precomputed_real_map.py'),
        copied_translation_units=copied,
        preparation='passenger_estimated_prepare.py SOURCE BUILD 544835fc59 --cpu; add boarding-failure fields to the copied packet digest',
        added_inspector_fields=['failure.service', 'failure.count', 'failure.since', 'failure.incarnation'],
        note='Only utils and evidence are modified. Exact-tick driver, paused preparation and read-only inspector use copied TUs; physical tracing is disabled.'))
    dump(output / 'evidence-sha256.json', {
        path.relative_to(output).as_posix(): sha(path)
        for path in sorted(output.rglob('*'))
        if path.is_file() and path.name != 'evidence-sha256.json'})


if __name__ == '__main__':
    main()
