"""Add local read-only client progress logging to a copied null-driver TU."""
import argparse
import pathlib
import re
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("source", type=pathlib.Path)
p.add_argument("build", type=pathlib.Path)
a = p.parse_args()
source, build = a.source.resolve(), a.build.resolve()
output = build / "network-probe"
output.mkdir(exist_ok=True)
text = (source / "src/video/null_v.cpp").read_text(encoding="utf-8")
text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (source / "src/video" / m[1]).resolve().as_posix() + '"', text)
anchor = 'extern std::atomic<bool> _exit_game;'
assert text.count(anchor) == 1
text = text.replace(anchor, anchor + '\n#include "' + (source / "src/date_func.h").as_posix() + '"\n#include "' + (source / "src/network/network.h").as_posix() + '"\nstatic bool pruning_probe = false;\nstatic int64_t next_pruning_probe = 0;')
anchor = '\tthis->until_exit = GetDriverParamBool(parm, "until_exit");'
assert text.count(anchor) == 1
text = text.replace(anchor, anchor + '\n\tpruning_probe = GetDriverParamBool(parm, "pruning_probe");')
anchor = '\t\t\t::UpdateWindows();'
assert text.count(anchor) == 2
probe = '''
            if (pruning_probe && _networking && !_network_server && _game_mode == GameMode::Normal && _state_ticks.base() >= next_pruning_probe) {
                next_pruning_probe = _state_ticks.base() + 128;
                IConsoleCmdExec("script progress-console.txt");
                IConsoleCmdExec("getfulldate");
                IConsoleCmdExec("passenger_journey_stats total");
                IConsoleCmdExec("script");
            }'''
text = text.replace(anchor, anchor + probe, 1)
target = output / "null_v.cpp"
target.write_text(text, encoding="utf-8")
project = build / "openttd_lib.vcxproj"
text = project.read_text(encoding="utf-8-sig")
original = str(source / "src/video/null_v.cpp")
assert text.count(original) >= 1
text = text.replace(original, str(target))
project.write_text(text, encoding="utf-8-sig")
print(target)
