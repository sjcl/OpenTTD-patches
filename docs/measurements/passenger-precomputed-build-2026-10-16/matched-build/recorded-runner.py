"""Reserve the shared host before building and measuring matching CRT/features."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'utils'))
import passenger_precomputed_real_map as real

build = root / 'build-runtime-matched'
data = root / 'build-msvc'
build.mkdir(exist_ok=True)
lock_path = pathlib.Path(tempfile.gettempdir()) / 'openttd-passenger-routing-measurement.lock'
print('Waiting for the existing shared measurement to finish; no process will be stopped.', flush=True)
lock = real.PROFILE.acquire_measurement_lock(lock_path, 3600)
os.write(lock, json.dumps(dict(pid=os.getpid(), output=str(build), purpose='matching build and sequential validation')).encode())

def command(arguments, log):
    print('Running: ' + ' '.join(map(str, arguments)), flush=True)
    with (build / log).open('wb') as stream:
        subprocess.run(list(map(str, arguments)), cwd=root, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, creationflags=subprocess.CREATE_NO_WINDOW)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def manifest(name):
    xml = ET.parse(build / 'openttd_lib.vcxproj')
    ns = {'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
    flags = {}
    for group in xml.findall('m:ItemDefinitionGroup', ns):
        if 'Release|x64' in group.get('Condition', ''):
            cl = group.find('m:ClCompile', ns)
            flags = {node.tag.split('}')[-1]:node.text for node in cl}
    assert flags['RuntimeLibrary'] == 'MultiThreaded', flags
    assert 'DEDICATED' not in flags['PreprocessorDefinitions'], flags
    assert 'WITH_ASSERT' in flags['PreprocessorDefinitions'], flags
    assert 'WITH_OPENGL' in flags['PreprocessorDefinitions'], flags
    product = ['src/passenger_precomputed.cpp','src/passenger_precomputed.h',
               'src/passenger_service_database.cpp','src/passenger_service_database.h','src/sl/extended_ver_sl.cpp']
    result = dict(source_head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
        product_sha256={p:sha(root/p) for p in product}, release_flags=flags,
        compiler=real.PROFILE.compiler_info(build),
        tools={p.name:sha(p) for p in (root/'utils').glob('passenger_precomputed_*.py')},
        copied_tus={str(p.relative_to(build)):sha(p) for folder in ['save-integrity','real-map-driver']
                    for p in (build/folder).glob('*') if p.is_file()},
        binary_sha256=sha(build/'Release/openttd.exe'))
    (build/name).write_text(json.dumps(result,indent=2)+'\n')

try:
    print('Shared host reservation acquired.', flush=True)
    real.PROFILE.wait_for_idle(300, 5)
    command(['cmake','-S',root,'-B',build,'-G','Visual Studio 17 2022','-A','x64',
             '-DCMAKE_PREFIX_PATH=C:/Users/bebe/CLionProjects/OpenTTD-patches/cmake-build-release-visual-studio/vcpkg_installed/x64-windows-static',
             '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded','-DOPTION_DEDICATED=OFF',
             '-DOPTION_USE_ASSERTS=ON','-DOPTION_NO_LTO=ON','-DOPTION_PASSENGER_BENCHMARK=OFF'], 'configure.log')
    revision = subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
    command([sys.executable,'-B',root/'utils/passenger_estimated_prepare.py',root,build,revision,'--cpu'], 'prepare-cpu.log')
    command(['cmake','--build',build,'--config','Release','--target','openttd','--parallel','12'], 'build-cpu.log')
    (build/'cpu-binary').mkdir(exist_ok=True)
    shutil.copy2(build/'Release/openttd.exe',build/'cpu-binary/openttd.exe')
    manifest('cpu-provenance.json')

    old = pathlib.Path('C:/Users/bebe/.codex/worktrees/dae2/OpenTTD-patches/build-estimated/cpu-through-final/openttd.exe')
    for label, binary, language, replacing in [
            ('old-steady',old,old.parent/'lang',False),
            ('new-steady',build/'cpu-binary/openttd.exe',data/'lang',False),
            ('old-retained',old,old.parent/'lang',True),
            ('new-retained',build/'cpu-binary/openttd.exe',data/'lang',True)]:
        print('Measuring matching configuration: ' + label, flush=True)
        args = argparse.Namespace(binary=binary,package=data/'package',output=data/('runtime-matched-'+label),
            input=data/'runtime-common-input/result.sav',lang=language,ticks=2048,timeout=900,resume=True,
            fixture_timetable=False,fixture_estimated=False,cold_precomputed=False,precomputed=True,
            warm_precomputed=not replacing,rebuild_precomputed=replacing,quality_probe=False,
            unpause_before_save=False,validation_only=False)
        real.run(args, measurement_lock_fd=lock)
    real.summarize(argparse.Namespace(output=data/'runtime-matched-summary.json',
        runs=[data/('runtime-matched-'+label) for label in ['old-steady','new-steady','old-retained','new-retained']]))

    command(['cmake','-S',root,'-B',build,'-DCMAKE_PROJECT_INCLUDE='], 'restore-before-mp.log')
    command([sys.executable,'-B',root/'utils/passenger_estimated_prepare.py',root,build,revision,'--mp'], 'prepare-mp.log')
    command(['cmake','--build',build,'--config','Release','--target','openttd','--parallel','12'], 'build-mp.log')
    (build/'mp-binary').mkdir(exist_ok=True)
    shutil.copy2(build/'Release/openttd.exe',build/'mp-binary/openttd.exe')
    manifest('mp-provenance.json')
    command(['cmake','-S',root,'-B',build,'-DCMAKE_PROJECT_INCLUDE='], 'restore-product.log')
    command(['cmake','--build',build,'--config','Release','--target','openttd','openttd_test','--parallel','12'], 'build-product.log')
    for name in real.PROFILE.ASSET_DIRS:
        source = data/'package'/name
        if source.is_dir():
            shutil.copytree(source, build/'Release'/name, dirs_exist_ok=True, copy_function=real.PROFILE.hardlink_or_copy)
    command(['ctest','--test-dir',build,'-C','Release','--output-on-failure','--parallel','4'], 'ctest.log')
    command([sys.executable,'-B',root/'utils/passenger_precomputed_join.py',build/'mp-binary/openttd.exe',
             data/'package',data/'toggle-mp-input-ready/result.sav',data/'matched-mp-toggle',data/'lang',
             '--port','17533','--frame-freq','20','--ticks','1024','--observe-timeout','300',
             '--initial-build','--toggle-precomputed'], 'mp-toggle.log')
    command([sys.executable,'-B',root/'utils/passenger_precomputed_diagnostic_readback.py',build/'mp-binary/openttd.exe',
             data/'package',data/'lang',data/'matched-readback',data/'runtime-matched-new-steady',
             data/'runtime-matched-new-retained'], 'readback.log')
    for project in ['openttd_lib.vcxproj','openttd_test.vcxproj']:
        xml = (build/project).read_text(encoding='utf-8-sig')
        assert not any(name in xml for name in ['save-integrity','real-map-driver','table-build'])
    (build/'completed.json').write_text(json.dumps(dict(success=True, product_sha256=sha(build/'Release/openttd.exe')))+'\n')
    print('All matching-configuration checks completed.', flush=True)
finally:
    os.close(lock)
    lock_path.unlink()
