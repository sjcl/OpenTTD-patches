"""Isolated layout prototype; restore both exact source bytes and normal build."""
import argparse, hashlib, json, os, pathlib, shutil, subprocess, sys, tempfile
import xml.etree.ElementTree as ET

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'utils'))
import passenger_precomputed_real_map as real
data = root / 'build-msvc'
build = root / 'build-runtime-matched'
out = data / 'cold-workspace-candidate'
files = ['passenger_precomputed.cpp', 'passenger_precomputed.h']
expected = ['d89fb2a43a0614a9255addcd31a9cdd69cf47e82d5aceec5fd06ba246883aaec',
            '8e51935b3f5855edca482da3c887bcba34bde7e83fdee8c95a6edd54123d4bd4']
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
lock_path = pathlib.Path(tempfile.gettempdir()) / 'openttd-passenger-routing-measurement.lock'
# The peer's next lease was observed before launching this script. This waits
# for the actual release; it makes no time-based reservation and stops nobody.
print('Peer lease observed. Waiting for actual release.', flush=True)
lock = real.PROFILE.acquire_measurement_lock(lock_path, 14400)
os.write(lock, json.dumps(dict(pid=os.getpid(), output=str(out), purpose='isolated cold-workspace layout validation')).encode())
backup = {}
applied = {}

def run(args, log):
    print('Running ' + log, flush=True)
    with (out / log).open('wb') as stream:
        subprocess.run(list(map(str, args)), cwd=root, stdout=stream,
                       stderr=subprocess.STDOUT, check=True,
                       creationflags=subprocess.CREATE_NO_WINDOW)

def configure(log):
    run(['cmake', '-S', root, '-B', build, '-DCMAKE_PROJECT_INCLUDE='], log)

try:
    real.PROFILE.wait_for_idle(300, 5)
    for name, wanted in zip(files, expected):
        p = root / 'src' / name
        assert sha(p) == wanted, (name, sha(p), wanted)
        backup[name] = p.read_bytes()
    (out / 'original').mkdir(exist_ok=True)
    for name in files:
        (out / 'original' / name).write_bytes(backup[name])
        target = root / 'src' / name
        target.write_bytes((out / name).read_bytes())
        applied[name] = sha(target)
    configure('configure.log')
    run(['cmake', '--build', build, '--config', 'Release', '--target',
         'openttd', 'openttd_test', '--parallel', '12'], 'build-tests.log')
    run(['ctest', '--test-dir', build, '-C', 'Release', '--output-on-failure',
         '--parallel', '4'], 'ctest.log')
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    run([sys.executable, '-B', root / 'utils/passenger_estimated_prepare.py',
         root, build, revision, '--cpu'], 'prepare-cpu.log')
    run(['cmake', '--build', build, '--config', 'Release', '--target',
         'openttd', '--parallel', '12'], 'build-cpu.log')
    binary = out / 'binary'
    binary.mkdir(exist_ok=True)
    shutil.copy2(build / 'Release/openttd.exe', binary / 'openttd.exe')
    ns = {'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
    for group in ET.parse(build / 'openttd_lib.vcxproj').findall('m:ItemDefinitionGroup', ns):
        if "=='Release|x64'" in group.attrib.get('Condition', ''):
            flags = {n.tag.split('}')[-1]:n.text for n in group.find('m:ClCompile', ns)}
    previous = json.loads((build / 'cpu-provenance.json').read_text())['release_flags']
    differences = {k:[previous.get(k), flags.get(k)] for k in previous.keys() | flags.keys()
                   if k != 'AdditionalIncludeDirectories' and previous.get(k) != flags.get(k)}
    assert not differences, differences
    provenance = dict(source_head=revision, product_sha256=applied, release_flags=flags,
                      release_flag_differences=differences, compiler=real.PROFILE.compiler_info(build),
                      binary_sha256=sha(binary / 'openttd.exe'),
                      copied_tus={str(p.relative_to(build)):sha(p)
                                  for folder in ['save-integrity', 'real-map-driver']
                                  for p in (build / folder).glob('*') if p.is_file()})
    (out / 'cpu-provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    checks = {}
    for label, replacing, baseline in [('steady', False, 'runtime-main-reverse-new'),
                                       ('retained', True, 'runtime-matched-new-retained')]:
        print('Measuring cold-workspace ' + label, flush=True)
        target = data / ('runtime-cold-workspace-' + label)
        args = argparse.Namespace(binary=binary / 'openttd.exe', package=data / 'package',
            output=target, input=data / 'runtime-common-input/result.sav', lang=data / 'lang',
            ticks=2048, timeout=900, resume=True, fixture_timetable=False, fixture_estimated=False,
            cold_precomputed=False, precomputed=True, warm_precomputed=not replacing,
            rebuild_precomputed=replacing, quality_probe=False, unpause_before_save=False,
            validation_only=False, affinity_mask=None, main_thread_affinity_mask=1)
        real.run(args, measurement_lock_fd=lock)
        old = json.loads((data / baseline / 'save-integrity.json').read_text())
        new = json.loads((target / 'save-integrity.json').read_text())
        mismatch = {k:[old.get(k),new.get(k)] for k in old.keys() | new.keys() if old.get(k) != new.get(k)}
        checks[label] = dict(reference=baseline, mismatches=mismatch)
        (out / 'canonical.json').write_text(json.dumps(checks, indent=2) + '\n')
        assert not mismatch, mismatch
    real.summarize(argparse.Namespace(output=data / 'runtime-cold-workspace-summary.json',
        runs=[data / ('runtime-cold-workspace-' + label) for label in ['steady','retained']]))
    (out / 'completed.json').write_text(json.dumps(dict(success=True, adopted=False)) + '\n')
    print('Layout experiment complete; restoring original source and normal build.', flush=True)
finally:
    try:
        for name in applied:
            target = root / 'src' / name
            assert sha(target) == applied[name], 'Unexpected concurrent edit: ' + name
            target.write_bytes(backup[name])
        if applied:
            configure('restore-configure.log')
            run(['cmake', '--build', build, '--config', 'Release', '--target',
                 'openttd', 'openttd_test', '--parallel', '12'], 'restore-build.log')
            for project in ['openttd_lib.vcxproj','openttd_test.vcxproj']:
                text = (build / project).read_text(encoding='utf-8-sig')
                assert not any(s in text for s in ['save-integrity','real-map-driver','table-build'])
            (out / 'restored.json').write_text(json.dumps(dict(product_sha256={n:sha(root/'src'/n) for n in files}))+'\n')
    finally:
        os.close(lock)
        lock_path.unlink()
