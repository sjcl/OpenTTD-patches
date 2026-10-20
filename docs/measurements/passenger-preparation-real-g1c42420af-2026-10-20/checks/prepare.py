"""Pinned baseline archive and copied measurement TUs; no product edits."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[4]
EVIDENCE = Path(__file__).resolve().parents[1]
WORK = ROOT / 'build-real-preparation-20261020'
BASE = '8f200282e83246898de702d014aa6acda0a95905'
NEW = 'fe663cfa81661fc02cddb30e5be1618dcea0292f'

def dump(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')

def run(command, log):
    command = list(map(str, command))
    dump(log.with_suffix('.command.json'), command)
    with log.open('wb') as out:
        subprocess.run(list(map(str, command)), cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, check=True)

def archive():
    WORK.mkdir(exist_ok=False)
    source = WORK / 'baseline-source'
    source.mkdir()
    top = subprocess.check_output(['git', 'ls-tree', '--name-only', BASE], cwd=ROOT, text=True).splitlines()
    paths = [p for p in top if p != 'docs'] + [
        'docs/measurements/passenger-routing-profile-2026-10-13/Run-Profile.py',
        'docs/measurements/passenger-query-work-admission-2026-10-14/Prepare-Save-Integrity.py']
    tar = WORK / 'baseline-source.tar'
    subprocess.run(['git', 'archive', '-o', str(tar), BASE, '--', *paths], cwd=ROOT, check=True)
    with tarfile.open(tar) as stream:
        stream.extractall(source, filter='data')
    (source / '.ottdrev').write_text(f'baseline-{BASE[:10]}\t20261020\t0\t{BASE}\t0\t0\n', encoding='utf-8')
    dump(EVIDENCE / 'source-identity.json', {'baseline': BASE, 'candidate': NEW,
        'baseline_src_tree': subprocess.check_output(['git', 'rev-parse', BASE + ':src'], text=True).strip(),
        'candidate_src_tree': subprocess.check_output(['git', 'rev-parse', NEW + ':src'], text=True).strip(),
        'archive_sha256': hashlib.file_digest(tar.open('rb'), 'sha256').hexdigest(),
        'baseline_source': str(source), 'candidate_source': str(ROOT)})
    configure('baseline')

def configure(label):
    build = WORK / 'baseline-build' if label == 'baseline' else ROOT / 'build-preparation'
    source = WORK / 'baseline-source' if label == 'baseline' else ROOT
    command = ['cmake', '-S', source, '-B', build, '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'ClangCL',
        '-DCMAKE_PROJECT_INCLUDE=', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
        '-DOPTION_USE_ASSERTS=ON', '-DOPTION_NO_LTO=ON', '-DOPTION_PGO=OFF', '-DOPTION_DEDICATED=OFF',
        '-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi', '-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi',
        '-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO',
        '-DCMAKE_TOOLCHAIN_FILE=C:/Users/bebe/.vcpkg-clion/vcpkg/scripts/buildsystems/vcpkg.cmake',
        '-DVCPKG_INSTALLED_DIR=C:/Users/bebe/CLionProjects/OpenTTD-patches/cmake-build-release-visual-studio/vcpkg_installed',
        '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
        '-DEDITBIN_EXECUTABLE=C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/editbin.exe',
        '-DVCPKG_MANIFEST_INSTALL=OFF']
    run(command, WORK / f'{label}-configure.log')

def instrument(label):
    source = WORK / 'baseline-source' if label == 'baseline' else ROOT
    build = WORK / 'baseline-build' if label == 'baseline' else ROOT / 'build-preparation'
    revision = BASE if label == 'baseline' else NEW
    original = (ROOT / 'utils/passenger_estimated_prepare.py').read_text(encoding='utf-8')
    old = "    anchor = '        const auto started = std::chrono::steady_clock::now();'"
    new = "    anchor = '        bool ready = false;\\n        const auto started = std::chrono::steady_clock::now();'"
    assert original.count(old) == 1
    compat = WORK / 'estimated_prepare_compat.py'
    compat.write_text(original.replace(old, new), encoding='utf-8', newline='\n')
    run([sys.executable, '-B', compat, source, build, revision, '--cpu'], WORK / f'{label}-prepare.log')
    run([sys.executable, '-B', ROOT / 'utils/passenger_wpr_prepare.py', build], WORK / f'{label}-wpr-prepare.log')
    # WPR tick and canonical hooks are identical for both versions. Additional
    # paused probes measure public preparation/replay APIs with OS CPU clocks.
    console = build / 'save-integrity/console_cmds.cpp'
    text = console.read_text(encoding='utf-8')
    extra = Path(__file__).with_name('probes.hpp').read_text(encoding='utf-8')
    anchor = 'static bool ConPassengerJourneyStats(std::span<std::string_view> argv)\n{'
    assert text.count(anchor) == 1
    text = text.replace(anchor, extra + '\n' + anchor)
    anchor = '    if (operation == "wait_saved_replay") {'
    assert text.count(anchor) == 1
    text = text.replace(anchor, '''    if (operation == "preparation_measure") return MeasurePassengerPreparation(false);
    if (operation == "replay_measure") return MeasurePassengerPreparation(true);
    if (operation == "measurement_wait_background") return WaitPassengerMeasurementBackground();
    if (operation == "mixed_replay_fixture") return MakePassengerReplayFixture();
''' + anchor)
    console.write_text(text, encoding='utf-8', newline='\n')
    # Include every generated TU in provenance, after the final instrumentation.
    dump(EVIDENCE / f'{label}-instrumentation.json', {
        'revision': revision, 'compatibility_change': 'Select only build_precomputed timer, as documented in the WPR runbook',
        'files': {p.relative_to(build).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for folder in ['save-integrity', 'real-map-driver'] for p in sorted((build / folder).glob('*')) if p.is_file()}})

def build(label):
    directory = WORK / 'baseline-build' if label == 'baseline' else ROOT / 'build-preparation'
    run(['cmake', '--build', directory, '--config', 'Release', '--target', 'openttd_test', 'openttd',
         '--parallel', '12', '--', '/nodeReuse:false', '/p:MultiProcCL=true', '/p:CL_MPCount=12'], WORK / f'{label}-build.log')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('action', choices=['archive', 'configure', 'instrument', 'build'])
    parser.add_argument('label', nargs='?', choices=['baseline', 'candidate'])
    args = parser.parse_args()
    if args.action == 'archive': archive()
    else: globals()[args.action](args.label)
