"""Freeze the single alternate run, MSVC checks and exact copied measurement sources."""
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
    run = args.build / 'alternate/run'
    process = json.loads((run / 'process.json').read_text(encoding='utf-8'))
    instrumentation = json.loads((args.build / 'real-map-instrumentation.json').read_text(encoding='utf-8'))

    def git(*arguments):
        return subprocess.check_output(['git', '-C', str(root), *arguments], text=True).strip()

    source_commit = git('rev-parse', instrumentation['source_revision'])
    if git('diff', source_commit, '--', 'src'):
        raise ValueError('Product sources differ from measured revision')
    if process['binary_sha256'] != sha(args.build / 'Release/openttd.exe'):
        raise ValueError('Binary changed')
    if process['language_sha256'] != sha(args.build / 'lang/english.lng'):
        raise ValueError('Language changed')

    def copy(source, target, compress=False):
        target.parent.mkdir(parents=True, exist_ok=True)
        if compress:
            target.write_bytes(gzip.compress(source.read_bytes(), mtime=0))
        else:
            shutil.copyfile(source, target)

    for filename in ('process.json', 'actual-ticks.json', 'save-integrity.json',
                     'midpoint-save-integrity.json', 'rebuild-transition.json'):
        copy(run / filename, output / 'run' / filename)
    for filename in ('stdout.txt', 'stderr.txt', 'on-1-console.txt'):
        copy(run / filename, output / 'run' / (filename + '.gz'), True)
    for script in (run / 'scripts').glob('*.scr'):
        copy(script, output / 'run/scripts' / script.name)
    copied = {}
    for subfolder in ('save-integrity', 'real-map-driver'):
        for path in sorted((args.build / subfolder).rglob('*')):
            if path.is_file() and path.suffix in ('.h', '.cpp'):
                relative = path.relative_to(args.build).as_posix()
                copied[relative] = sha(path)
                copy(path, output / 'copied-tus' / (relative + '.gz'), True)
    copy(args.build / 'native-alternate-all.log', output / 'checks/native-all.log')
    copy(args.build / 'build-alternate.log', output / 'checks/build.log.gz', True)
    copy(args.build / 'real-map-instrumentation.json', output / 'checks/real-map-instrumentation.json')
    package = json.loads((args.package / 'package.json').read_text(encoding='utf-8'))
    if sha(args.package / 'input.sav') != process['input_sha256'] or process['input_sha256'] != package['input_sha256']:
        raise ValueError('Input changed')
    for asset in package['assets']:
        if sha(args.package / asset['path']) != asset['sha256']:
            raise ValueError('Asset changed: ' + asset['path'])
    copy(args.package / 'package.json', output / 'package.json')
    copy(args.package / 'openttd.cfg', output / 'openttd.cfg')
    dump(output / 'source-identity.json', dict(
        commit=source_commit, product_src_tree=git('rev-parse', source_commit + ':src'),
        product_sources_unchanged=True, binary_sha256=process['binary_sha256'],
        test_binary_sha256=sha(args.build / 'Release/openttd_test.exe'),
        runner_sha256=sha(root / 'utils/passenger_precomputed_real_map.py'),
        copied_translation_units=copied,
        preparation='passenger_estimated_prepare.py SOURCE BUILD 594bbecad3 --cpu; add failure.service/count/since/incarnation to copied packet digest',
        note='Release/assert ON, static CRT, LTO OFF. Product sources unchanged. Timing is from the product walk counters; exact tick driving and read-only integrity use copied TUs.'))
    dump(output / 'evidence-sha256.json', {
        path.relative_to(output).as_posix(): sha(path) for path in sorted(output.rglob('*'))
        if path.is_file() and path.name != 'evidence-sha256.json'})


if __name__ == '__main__':
    main()
