"""Observe product GUI preparation and the actual first network sync, copied TUs only."""
from prepare import ROOT, WORK, EVIDENCE, dump
from pathlib import Path
import argparse
import re
import hashlib

parser = argparse.ArgumentParser(__doc__)
parser.add_argument('label', choices=['baseline', 'candidate'])
args = parser.parse_args()
source = WORK / 'baseline-source' if args.label == 'baseline' else ROOT
build = WORK / 'baseline-build' if args.label == 'baseline' else ROOT / 'build-preparation'
folder = build / 'waiting-probe'; folder.mkdir(exist_ok=True)
header = folder / 'waiting.hpp'
text = Path(__file__).with_name('waiting.hpp').read_text(encoding='utf-8')
text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (source / 'src' / m[1]).as_posix() + '"', text)
header.write_text(text, encoding='utf-8', newline='\n')
include = '#include "' + header.as_posix() + '"\n'

def update(path, anchors):
    text = path.read_text(encoding='utf-8')
    for old, new in anchors:
        assert text.count(old) == 1, old
        text = text.replace(old, new)
    path.write_text(text, encoding='utf-8', newline='\n')

update(build / 'save-integrity/passenger_service_database.cpp', [
    ('namespace {', include + '\nnamespace {'),
    ('bool PassengerServiceDatabase::PreparePrecomputed()\n{', 'bool PassengerServiceDatabase::PreparePrecomputed()\n{\n    WaitingProbe::Scope observation_scope{*this};'),
    ('PassengerServiceDatabase::LoadState PassengerServiceDatabase::StepLoad(size_t allowance)\n{',
     'PassengerServiceDatabase::LoadState PassengerServiceDatabase::StepLoad(size_t allowance)\n{\n    WaitingProbe::Scope observation_scope{*this};')])
update(build / 'save-integrity/console_cmds.cpp', [
    ('static bool ConPassengerJourneyStats(std::span<std::string_view> argv)\n{', include + 'static bool ConPassengerJourneyStats(std::span<std::string_view> argv)\n{'),
    ('    if (operation == "preparation_measure")', '    if (operation == "waiting_arm") { WaitingProbe::Arm(); return true; }\n    if (operation == "preparation_measure")')])

def copy(relative, old, new):
    original = source / 'src' / relative
    text = original.read_text(encoding='utf-8')
    text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"'
        if (original.parent / m[1]).is_file() else m[0], text)
    assert text.count(old) == 1
    text = text.replace(old, new)
    line = next(line for line in text.splitlines(keepends=True) if line.startswith('#include ') and line.rstrip().endswith('/stdafx.h"'))
    text = text.replace(line, line + include, 1)
    target = folder / original.name
    target.write_text(text, encoding='utf-8', newline='\n')
    project = build / 'openttd_lib.vcxproj'
    xml = project.read_text(encoding='utf-8-sig')
    assert xml.count(str(original)) == 1
    project.write_text(xml.replace(str(original), str(target)), encoding='utf-8-sig', newline='\n')

copy('openttd.cpp', '\n\tif (_networking && !HasModalProgress()) {\n\t\t/* Multiplayer */',
    '\n    if (!_networking && _game_mode == GameMode::Normal && WaitingProbe::Complete("singleplayer")) return;\n\tif (_networking && !HasModalProgress()) {\n\t\t/* Multiplayer */')
copy('network/network_client.cpp',
    'if (_network_join_status != NetworkJoinStatus::Registering) CloseWindowById(WindowClass::NetworkStatus, NetworkStatusWindowNumber::Join);',
    'if (_network_join_status != NetworkJoinStatus::Registering) CloseWindowById(WindowClass::NetworkStatus, NetworkStatusWindowNumber::Join);\n                WaitingProbe::Complete("multiplayer");')
dump(EVIDENCE / f'{args.label}-waiting-instrumentation.json', {
    'scope': 'Copied translation units only; observe public preparation APIs and first sync; exit singleplayer before StateGameLoop',
    'files': {p.relative_to(build).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for name in ['save-integrity', 'real-map-driver', 'waiting-probe'] for p in sorted((build / name).glob('*')) if p.is_file()}})
