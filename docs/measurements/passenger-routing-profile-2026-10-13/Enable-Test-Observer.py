"""Enable detail observations in the existing regression tests, in generated files only."""
import argparse
import pathlib
import os

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=pathlib.Path)
parser.add_argument('--source', type=pathlib.Path, default=pathlib.Path(os.path.abspath(__file__)).parents[3])
args = parser.parse_args()
directory = args.build / 'journey-profile'
if not (directory / 'profile.h').exists():
    parser.error('Build detail mode first')
source = directory / 'test_observer.cpp'
observer = '#include "' + (args.source / 'src/stdafx.h').as_posix() + '"\n#include "profile.h"\nnamespace { struct EnableObserver { EnableObserver() { JourneyProfile::enabled = true; } } enable_observer; }\n'
if 'template <Name name> inline Metric &Site()' in (directory / 'profile.h').read_text(encoding='utf-8'):
    observer += '#include "' + (args.source / 'src/3rdparty/catch2/catch.hpp').as_posix() + '"\n'
    observer += r'''
TEST_CASE("Journey profile counter reset and nested elapsed scopes", "[passenger][journey-profile]")
{
    using namespace JourneyProfile;
    REQUIRE(active_scope == nullptr);
    Reset(); Count<"probe.count">();
    REQUIRE(Get("probe.count").calls == 1);
    Reset(); Count<"probe.count">(); Count<"probe.count">();
    REQUIRE(Get("probe.count").calls == 2);
    Reset(); enabled = false;
    { FixedScope<"probe.disabled"> scope; Count<"probe.count">(); }
    REQUIRE(metrics.empty());
    REQUIRE(active_scope == nullptr);
    Reset();
    { FixedScope<"probe.parent"> parent; { FixedScope<"probe.child"> child; Count<"probe.count">(); } }
    REQUIRE(active_scope == nullptr);
    REQUIRE(Get("probe.parent").calls == 1);
    REQUIRE(Get("probe.child").calls == 1);
    REQUIRE(Get("probe.parent").ns == Get("probe.parent").exclusive_ns + Get("probe.child").ns);
    Reset();
}
'''
source.write_text(observer, encoding='utf-8')
project = args.build / 'openttd_test.vcxproj'
text = project.read_text(encoding='utf-8-sig')
entry = f'<ItemGroup><ClCompile Include="{source}" /></ItemGroup>\n'
text = text.replace(entry, '')
before, closing, after = text.rpartition('</Project>')
if not closing:
    parser.error('Generated project closing element missing')
text = before + entry + closing + after
project.write_text(text, encoding='utf-8-sig')
