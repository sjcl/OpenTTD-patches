#!/usr/bin/env python3
"""Generate an opt-in immutable pre-parallel reference TU for native comparisons.

Configure with -DCMAKE_PROJECT_INCLUDE=<build>/parallel-reference/reference.cmake.
The generated code is linked only into openttd_test, never into the game.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = "093218f882"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--reference", default=BASE)
    args = parser.parse_args()
    build = args.build.resolve() / "parallel-reference"
    build.mkdir(parents=True, exist_ok=True)
    revision = subprocess.check_output(["git", "rev-parse", args.reference], cwd=ROOT, text=True).strip()
    def read(name):
        return subprocess.check_output(["git", "show", f"{revision}:src/{name}"], cwd=ROOT).decode("utf-8")
    header = read("passenger_precomputed.h")
    header = header[header.index("class PassengerPrecomputedTable {"):header.rindex("#endif")]
    header = header.replace("PassengerPrecomputedTable", "PassengerSequentialReference").replace("private:", "public:")
    (build / "PassengerSequentialReference.h").write_text(
        '#pragma once\n#include "' + (ROOT / "src/passenger_precomputed.h").as_posix() + '"\n' + header, encoding="utf-8")
    cpp = read("passenger_precomputed.cpp").replace("PassengerPrecomputedTable", "PassengerSequentialReference")
    cpp = cpp.replace('#include "passenger_precomputed.h"', '#include "PassengerSequentialReference.h"')
    cpp = re.sub(r'#include "(?!PassengerSequentialReference.h)([^"]+)"',
                 lambda m: '#include "' + (ROOT / "src" / m[1]).as_posix() + '"', cpp)
    (build / "PassengerSequentialReference.cpp").write_text(cpp, encoding="utf-8")
    (build / "reference.cmake").write_text(
        'function(add_passenger_parallel_reference)\n'
        f'  target_sources(openttd_test PRIVATE "{build.as_posix()}/PassengerSequentialReference.cpp")\n'
        f'  target_include_directories(openttd_test PRIVATE "{build.as_posix()}")\n'
        '  target_compile_definitions(openttd_test PRIVATE WITH_PASSENGER_PARALLEL_REFERENCE)\n'
        'endfunction()\ncmake_language(DEFER CALL add_passenger_parallel_reference)\n', encoding="utf-8")
    (build / "source.json").write_text(json.dumps(dict(reference=revision), indent=2) + "\n", encoding="utf-8")
    print(build / "reference.cmake")


if __name__ == "__main__":
    main()
