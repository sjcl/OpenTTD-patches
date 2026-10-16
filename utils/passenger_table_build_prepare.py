#!/usr/bin/env python3
"""Generate isolated, instrumented reference/candidate TUs; never alter product TUs.

Configure with -DCMAKE_PROJECT_INCLUDE=BUILD/table-build/benchmark.cmake after
running this script. Reconfigure without that option to restore the product.
The reference is pinned to the requested pre-optimization commit.
"""
import argparse
import pathlib
import re
import subprocess

BASE = "902997e6314e018a08c4b92ec0083b040790005f"
ROOT = pathlib.Path(__file__).resolve().parents[1]


def generate(source, build, name, revision):
    def read(path):
        if revision:
            return subprocess.check_output(["git", "show", f"{revision}:src/{path}"], cwd=source).decode()
        return (source / "src" / path).read_text(encoding="utf-8")

    header = read("passenger_precomputed.h")
    header = header[header.index("class PassengerPrecomputedTable {"):header.rindex("#endif")]
    header = header.replace("PassengerPrecomputedTable", name).replace("private:", "public:")
    metrics = """
    struct Metrics {
        uint64_t relaxations = 0, successes = 0, pushes = 0, pops = 0, stale = 0, peak_queue = 0, copies = 0;
        uint64_t section_checks = 0, graph_ns = 0, init_ns = 0, search_ns = 0, finish_ns = 0, publish_ns = 0;
        std::chrono::steady_clock::time_point phase;
        std::vector<uint64_t> destinations_ns;
    } metrics;
"""
    header = header.replace("public:", "public:\n" + metrics, 1)
    (build / f"{name}.h").write_text('#pragma once\n#include <chrono>\n#include "' + (source / "src/passenger_precomputed.h").as_posix() + '"\n' + header, encoding="utf-8")
    cpp = read("passenger_precomputed.cpp").replace("PassengerPrecomputedTable", name)
    cpp = cpp.replace('#include "passenger_precomputed.h"', f'#include "{name}.h"')
    cpp = re.sub(r'#include "([^"/]+)"', lambda m: m[0] if m[1] == name + '.h' else '#include "' + (source / "src" / m[1]).as_posix() + '"', cpp)
    cpp = cpp.replace("this->queue.emplace(", "++this->metrics.pushes; this->queue.emplace(")
    cpp = cpp.replace("this->queue.pop();", "this->queue.pop(); ++this->metrics.pops;")
    cpp = cpp.replace("\t\t\t++used;", "\t\t\t++used; this->metrics.peak_queue = std::max<uint64_t>(this->metrics.peak_queue, this->queue.size());")
    cpp = cpp.replace("Label next = ", "++this->metrics.copies; Label next = ")
    cpp = cpp.replace("\t\t\t\told = next;", "\t\t\t\t++this->metrics.successes; old = next;")
    cpp = cpp.replace("this->SetSearchLabel(edge.from, next);", "++this->metrics.successes; this->SetSearchLabel(edge.from, next);")
    cpp = cpp.replace("\t\t\t\tconst auto &edge =", "\t\t\t\t++this->metrics.relaxations; const auto &edge =")
    cpp = cpp.replace("\t\t\t\t\tconst auto &ride = this->section_rides[this->finishing", "\t\t\t\t\t++this->metrics.section_checks; const auto &ride = this->section_rides[this->finishing")
    cpp = cpp.replace("legs != this->scratch.labels[state].legs) continue;", "legs != this->scratch.labels[state].legs) { ++this->metrics.stale; continue; }")
    cpp = cpp.replace("legs != this->SearchLegs(state)) continue;", "legs != this->SearchLegs(state)) { ++this->metrics.stale; continue; }")
    cpp = cpp.replace("\tthis->scratch.labels.clear();", "\tthis->metrics.phase = Clock::now();\n\tthis->scratch.labels.clear();")
    cpp = cpp.replace("else this->initializing_row = false;", "else { this->initializing_row = false; this->metrics.init_ns += Elapsed(this->metrics.phase); this->metrics.phase = Clock::now(); }")
    cpp = cpp.replace("this->finishing_row = true; continue;", "this->finishing_row = true; this->metrics.search_ns += Elapsed(this->metrics.phase); this->metrics.phase = Clock::now(); continue;")
    cpp = cpp.replace("\t\t\t\t\t/* Arrival/departure labels", "\t\t\t\t\tif (this->prepared.labels.empty()) { this->metrics.finish_ns += Elapsed(this->metrics.phase); this->metrics.phase = Clock::now(); }\n\t\t\t\t\t/* Arrival/departure labels")
    cpp = cpp.replace("\t\t\t\t\t++this->destination;", "\t\t\t\t\tthis->metrics.publish_ns += Elapsed(this->metrics.phase);\n\t\t\t\t\t++this->destination;")
    (build / f"{name}.cpp").write_text(cpp, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("--source", type=pathlib.Path, default=ROOT)
    parser.add_argument("--candidate", default=None, help="Git ref; default: working tree")
    args = parser.parse_args()
    build = args.build.resolve() / "table-build"
    build.mkdir(parents=True, exist_ok=True)
    generate(args.source.resolve(), build, "PassengerTableReference", BASE)
    generate(args.source.resolve(), build, "PassengerTableCandidate", args.candidate)
    sources = [build / "PassengerTableReference.cpp", build / "PassengerTableCandidate.cpp", ROOT / "utils/passenger_table_build_benchmark.cpp"]
    cmake = 'function(add_table_build_benchmark)\n  target_sources(openttd_test PRIVATE\n' + ''.join(f'    "{p.as_posix()}"\n' for p in sources) + ')\n  target_include_directories(openttd_test PRIVATE "' + build.as_posix() + '")\nendfunction()\ncmake_language(DEFER CALL add_table_build_benchmark)\n'
    (build / "benchmark.cmake").write_text(cmake, encoding="utf-8")
    print(build / "benchmark.cmake")


if __name__ == "__main__":
    main()
