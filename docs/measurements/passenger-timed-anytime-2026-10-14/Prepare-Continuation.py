"""Count actual simulation ticks and resume confirmed NewGRF error pauses.

Only the isolated continuation benchmark uses this copied null driver. No
production source, capacity, timetable, routing Work or passenger check changes.
"""
import argparse
import hashlib
import json
import pathlib
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("source", type=pathlib.Path)
p.add_argument("build", type=pathlib.Path)
a = p.parse_args()
source, build = a.source.resolve(), a.build.resolve()
original = source / "src/video/null_v.cpp"
text = original.read_text(encoding="utf-8")
text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (source / "src/video" / m[1]).resolve().as_posix() + '"', text)
headers = '#include <charconv>\n#include <fstream>\n#include "' + (source / "src/openttd.h").as_posix() + '"\n#include "' + (source / "src/date_func.h").as_posix() + '"\n'
# OpenTTD's platform types and attributes must be defined before its headers.
stdafx = '#include "' + (source / "src/stdafx.h").as_posix() + '"\n'
assert text.count(stdafx) == 1
text = text.replace(stdafx, stdafx + headers)
anchor = "\t\tfor (int i = 0; i < this->ticks; i++) {\n\t\t\t::GameLoop();\n\t\t\t::InputLoop();\n\t\t\t::UpdateWindows();\n\t\t}"
assert text.count(anchor) == 1
text = text.replace(anchor, '''
        int64_t start = 0;
        bool started = false;
        uint64_t loops = 0, error_resumes = 0;
        while (!_exit_game && (!started || _state_ticks.base() - start < this->ticks)) {
            if (started && _pause_mode.Test(PauseMode::Error)) {
                ++error_resumes;
                IConsoleCmdExec("unpause");
            }
            ::GameLoop();
            ::InputLoop();
            ::UpdateWindows();
            ++loops;
            if (!started && _game_mode == GameMode::Normal) {
                // Loading occurs inside GameLoop. Use the harness's clock from
                // game_start, before its first simulation tick, rather than
                // the intro game's clock or the already advanced live clock.
                std::ifstream clock("on-1-console.txt");
                std::string line;
                while (std::getline(clock, line)) {
                    constexpr std::string_view prefix = "State ticks: ";
                    if (!line.starts_with(prefix)) continue;
                    const auto parsed = std::from_chars(line.data() + prefix.size(), line.data() + line.size(), start);
                    if (parsed.ec != std::errc{}) continue;
                    started = true;
                    break;
                }
                if (!started) {
                    Debug(misc, 0, "Missing continuation game_start StateTicks");
                    _exit_game = true;
                }
            }
        }
        std::ofstream out("continuation-driver.json");
        out << "{\\"start_tick\\":" << start << ",\\"end_tick\\":" << _state_ticks.base()
            << ",\\"driver_loops\\":" << loops << ",\\"error_resumes\\":" << error_resumes << "}\\n";
''')
target = build / "continuation-driver/null_v.cpp"
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(text, encoding="utf-8")
project = build / "openttd_lib.vcxproj"
xml = project.read_text(encoding="utf-8-sig")
assert xml.count(str(original)) >= 1
project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")
metadata_path = build / "journey-profile/instrumentation.json"
metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
metadata["state_tick_driver"] = {"original_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
    "copy_sha256": hashlib.sha256(target.read_bytes()).hexdigest(), "benchmark_error_resume": True,
    "start_clock": "game_start StateTicks in the fresh on-1-console.txt, before its first simulation tick",
    "scope": "isolated continuation only; excluded from initial performance comparisons"}
metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
print(target)
