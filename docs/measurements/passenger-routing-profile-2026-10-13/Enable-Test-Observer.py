"""Enable detail observations in the existing regression tests, in generated files only."""
import argparse
import pathlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=pathlib.Path)
args = parser.parse_args()
directory = args.build / 'journey-profile'
if not (directory / 'profile.h').exists():
    parser.error('Build detail mode first')
source = directory / 'test_observer.cpp'
source.write_text('#include "profile.h"\nnamespace { struct EnableObserver { EnableObserver() { JourneyProfile::enabled = true; } } enable_observer; }\n', encoding='utf-8')
project = args.build / 'openttd_test.vcxproj'
text = project.read_text(encoding='utf-8-sig')
entry = f'<ItemGroup><ClCompile Include="{source}" /></ItemGroup>\n'
text = text.replace(entry, '')
before, closing, after = text.rpartition('</Project>')
if not closing:
    parser.error('Generated project closing element missing')
text = before + entry + closing + after
project.write_text(text, encoding='utf-8-sig')
