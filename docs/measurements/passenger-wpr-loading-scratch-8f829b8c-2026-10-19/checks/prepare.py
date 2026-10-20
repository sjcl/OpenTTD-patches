"""Prepare exact-revision normal-condition WPR captures."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = Path(__file__).resolve().parent
BUILD = ROOT / 'build-wpr-97cdc0ad/compile'
PREVIOUS = ROOT / 'build-wpr-0b8a21c0'
EVIDENCE = ROOT / 'docs/measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
REVISION = '8f829b8c3d5f35cb420c2a16812716ef24e38939'

def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def link_asset(source, target):
    if Path(source).name in {'input.sav', 'openttd.cfg', 'package.json'}:
        return shutil.copy2(source, target)
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)
    return str(target)

def freeze():
    identity = load(EVIDENCE / 'input-identity.json')
    manifest = load(EVIDENCE / 'input/package.json')
    assert sha(PREVIOUS / 'input/package.json') == identity['baseline_package_manifest_sha256']
    assert sha(PREVIOUS / 'input/input.sav') == identity['input_sha256']
    assert sha(PREVIOUS / 'input/openttd.cfg') == manifest['config_sha256']
    assert all(sha(PREVIOUS / 'input' / item['path']) == item['sha256'] for item in manifest['assets'])
    shutil.copytree(PREVIOUS / 'input', WORK / 'input', copy_function=link_asset)
    assert sha(WORK / 'input/input.sav') == identity['input_sha256']
    assert all(sha(WORK / 'input' / item['path']) == item['sha256'] for item in manifest['assets'])
    (WORK / 'input/input.sav').chmod(stat.S_IREAD)
    identity.update(same_as_0b8a21c0=True, source_package=str(PREVIOUS / 'input'),
                    manifest_sha256=sha(WORK / 'input/package.json'))
    dump(WORK / 'input-identity.json', identity)
    print('Frozen input: save/cfg/all 245 assets verified against the retained evidence', flush=True)

def build():
    assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip() == REVISION
    assert not subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip()
    env = {key: value for key, value in os.environ.items() if not key.startswith('OPENTTD_')}
    config = load(EVIDENCE / 'build-command.json')
    config['configure_command'][2] = str(ROOT)
    config['configure_command'][4] = str(BUILD)
    config['checkout_commit'] = config['measurement_target'] = REVISION
    config['native_test_command'][0] = str(BUILD / 'Release/openttd_test.exe')
    dump(WORK / 'build-command.json', config)
    original = ROOT / 'utils/passenger_estimated_prepare.py'
    source = original.read_text(encoding='utf-8')
    before = "    anchor = '        const auto started = std::chrono::steady_clock::now();'"
    after = "    anchor = '        bool ready = false;\\n        const auto started = std::chrono::steady_clock::now();'"
    assert source.count(before) == 1
    compatibility = WORK / 'estimated_prepare_compat.py'
    compatibility.write_text(source.replace(before, after), encoding='utf-8')
    dump(WORK / 'prepare-compat.json', dict(original=str(original), original_sha256=sha(original),
         measurement_copy=str(compatibility), copy_sha256=sha(compatibility), before=before, after=after,
         reason='Select the build_precomputed timer after wait_saved_replay added another timer.',
         product_sources_unchanged=True))
    with (WORK / 'configure.log').open('wb') as output:
        subprocess.run(config['configure_command'], cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT, check=True)
    subprocess.run([sys.executable, '-B', str(compatibility), str(ROOT), str(BUILD), REVISION, '--cpu'],
                   cwd=ROOT, env=env, check=True)
    subprocess.run([sys.executable, '-B', str(ROOT / 'utils/passenger_wpr_prepare.py'), str(BUILD)],
                   cwd=ROOT, env=env, check=True)
    with (WORK / 'build.log').open('wb') as output:
        subprocess.run(['cmake', '--build', str(BUILD), '--config', 'Release', '--target', 'openttd_test',
                        'openttd', '--parallel', '12', '--', '/nodeReuse:false'], cwd=ROOT, env=env,
                       stdout=output, stderr=subprocess.STDOUT, check=True)
    with (WORK / 'native-all.log').open('wb') as output:
        subprocess.run(config['native_test_command'], cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT, check=True)
    print((WORK / 'native-all.log').read_text(errors='replace')[-500:], flush=True)
    identity = dict(product_commit=REVISION,
                    src_tree=subprocess.check_output(['git', 'rev-parse', 'HEAD:src'], cwd=ROOT, text=True).strip(), files={})
    (WORK / 'Release').mkdir()
    for name in ['openttd.exe', 'openttd.pdb', 'openttd_test.exe']:
        source = BUILD / 'Release' / name
        target = WORK / 'Release' / name
        shutil.copy2(source, target)
        assert sha(source) == sha(target)
        identity['files']['Release/' + name] = dict(bytes=target.stat().st_size, sha256=sha(target))
    for folder in ['lang', 'baseset', 'save-integrity', 'real-map-driver']:
        shutil.copytree(BUILD / folder, WORK / folder)
    identity.update(instrumentation='Same copied-TU exact-tick/canonical/WPR helpers as 0b8a21c0; product sources unchanged',
                    compiled_from=str(BUILD))
    dump(WORK / 'build-identity.json', identity)
    (WORK / 'build-ready.txt').write_text('Exact revision built and native tests passed\n')
    print('8f829b8c executable/PDB prepared', flush=True)

if __name__ == '__main__':
    {'freeze': freeze, 'build': build}[sys.argv[1]]()
