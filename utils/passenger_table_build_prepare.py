#!/usr/bin/env python3
"""Generate isolated, instrumented reference/candidate TUs; never alter product TUs.

Configure with -DCMAKE_PROJECT_INCLUDE=BUILD/table-build/benchmark.cmake after
running this script. Reconfigure without that option to restore the product.
The reference is pinned to the requested pre-optimization commit.
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import textwrap

BASE = "902997e6314e018a08c4b92ec0083b040790005f"
ROOT = pathlib.Path(__file__).resolve().parents[1]


def split_bounded_spans_simd(cpp):
    """Vectorize the two hot spans while retaining the shared cold accept path."""
    pattern = re.compile(r'(?P<indent>\t+)for \(uint32_t from = (?P<first>[^;\n]+); from < (?P<last>\w+); \+\+from\) \{\n'
        r'(?P=indent)\tif \(cost < this->search.cost\[from\] \|\| \(cost == this->search.cost\[from\] && legs < this->search.legs\[from\]\)\) accept\(from\);\n(?P=indent)\}')
    def vectorize(match):
        indent, first, last = (match[key] for key in ('indent', 'first', 'last'))
        body = '''{
    uint32_t from = FIRST;
    for (; LAST - from >= 4; from += 4) {
        uint32_t mask = PassengerRangeMask(cost, legs, this->search.cost.data() + from, this->search.legs.data() + from);
        while (mask != 0) { unsigned bit = std::countr_zero(mask); mask &= mask - 1; accept(from + bit); }
    }
    for (; from < LAST; ++from) {
        if (cost < this->search.cost[from] || (cost == this->search.cost[from] && legs < this->search.legs[from])) accept(from);
    }
}'''.replace('FIRST', first).replace('LAST', last).replace('    ', '\t')
        return textwrap.indent(body, indent)
    cpp, count = pattern.subn(vectorize, cpp)
    assert count == 2, count
    return cpp


def generate(source, build, name, revision, heap="priority", simd=False, contract=False, generation=False, snapshot=None):
    def read(path):
        if snapshot is not None:
            return (snapshot / (path + ".txt")).read_text(encoding="utf-8")
        if revision:
            return subprocess.check_output(["git", "show", f"{revision}:src/{path}"], cwd=source).decode()
        return (source / "src" / path).read_text(encoding="utf-8")

    header = read("passenger_precomputed.h")
    header = header[header.index("class PassengerPrecomputedTable {"):header.rindex("#endif")]
    header = header.replace("PassengerPrecomputedTable", name).replace("private:", "public:")
    native_queue = "PassengerDestinationQueue<QueueItem> queue;" in header and heap != "product"
    if native_queue:
        header = header.replace("PassengerDestinationQueue<QueueItem> queue;", "std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;")
    header = header.replace("std::greater<QueueItem>", "PassengerQueueGreater<QueueItem>")
    if heap not in ("priority", "product"):
        queue = dict(indexed="PassengerIndexedQueue", radix="PassengerRadixQueue", four="PassengerFourQueue", packed="PassengerPackedFourQueue")[heap]
        header = re.sub(r"std::priority_queue<QueueItem,.*?> queue;", queue + "<QueueItem> queue;", header)
    metrics = """
    struct Metrics {
        uint64_t relaxations = 0, successes = 0, pushes = 0, pops = 0, stale = 0, peak_queue = 0, copies = 0;
        uint64_t section_checks = 0, graph_ns = 0, sections_ns = 0, csr_ns = 0, init_ns = 0, search_ns = 0, finish_ns = 0, publish_ns = 0;
        uint64_t heap_ns = 0, heap_samples = 0;
        uint64_t push_order = 14695981039346656037ULL, pop_order = 14695981039346656037ULL;
        uint64_t range_comparisons = 0, invariant_comparisons = 0;
        std::chrono::steady_clock::time_point phase, row_started;
        std::vector<uint64_t> destinations_ns;
    };
    mutable Metrics metrics;
    template <typename... Args> void MeasuredPush(Args... args)
    {
        if constexpr (PASSENGER_TABLE_COUNT) {
            std::apply([&](auto... value) { ((this->metrics.push_order = (this->metrics.push_order ^ uint64_t(value)) * 1099511628211ULL), ...); }, QueueItem{args...});
            if ((++this->metrics.pushes & 1023) == 0) {
                const auto started = std::chrono::steady_clock::now(); this->queue.emplace(args...);
                this->metrics.heap_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
                ++this->metrics.heap_samples; return;
            }
        }
        this->queue.emplace(args...);
    }
    auto MeasuredPop()
    {
        if constexpr (PASSENGER_TABLE_COUNT) {
            std::apply([&](auto... value) { ((this->metrics.pop_order = (this->metrics.pop_order ^ uint64_t(value)) * 1099511628211ULL), ...); }, this->queue.top());
            if ((++this->metrics.pops & 1023) == 0) {
                const auto started = std::chrono::steady_clock::now(); auto item = this->queue.top(); this->queue.pop();
                this->metrics.heap_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
                ++this->metrics.heap_samples; return item;
            }
        }
        auto item = this->queue.top(); this->queue.pop(); return item;
    }
"""
    if generation:
        header = header.replace("std::vector<int64_t> cost;", "std::vector<uint32_t> generation;\n\t\tstd::vector<int64_t> cost;")
        header = header.replace("size_t destination = 0, cursor = 0;", "uint32_t generation_epoch = 0; size_t init_count = 0;\n\tsize_t destination = 0, cursor = 0;")
        header = header.replace("this->optimized ? this->search.cost[state] :", "this->optimized ? (this->search.generation[state] == this->generation_epoch ? this->search.cost[state] : INT64_MAX) :")
        header = header.replace("this->optimized ? this->search.legs[state] :", "this->optimized ? (this->search.generation[state] == this->generation_epoch ? this->search.legs[state] : 0) :")
    header = header.replace("public:", "public:\n" + metrics, 1)
    (build / f"{name}.h").write_text('#pragma once\n#include <chrono>\n#include "' + (source / "src/passenger_precomputed.h").as_posix() + '"\n#include "' + (ROOT / "utils/passenger_table_build_heaps.h").as_posix() + '"\n' + header, encoding="utf-8")
    cpp = read("passenger_precomputed.cpp").replace("PassengerPrecomputedTable", name)
    if generation:
        cpp = cpp.replace("this->search.cost.clear(); this->search.legs.clear();", "++this->generation_epoch; this->init_count = 0; this->search.generation.resize(this->diagnostic.states);")
        cpp = cpp.replace("this->search.cost.reserve(this->diagnostic.states); this->search.legs.reserve(this->diagnostic.states);", "this->search.cost.resize(this->diagnostic.states); this->search.legs.resize(this->diagnostic.states);")
        cpp = cpp.replace("this->search.cost.size() < this->diagnostic.states", "this->init_count < this->diagnostic.states")
        cpp = cpp.replace("this->diagnostic.states - this->search.cost.size()", "this->diagnostic.states - this->init_count")
        cpp = cpp.replace("this->search.cost.resize(this->search.cost.size() + count, INT64_MAX);", "this->init_count += count;")
        cpp = cpp.replace("this->search.legs.resize(this->search.legs.size() + count, 0);", "")
        cpp = cpp.replace("if (this->search.cost[state] == INT64_MAX)", "if (this->SearchCost(state) == INT64_MAX)")
        cpp = cpp.replace("this->search.cost[state] = label.cost;", "this->search.generation[state] = this->generation_epoch; this->search.cost[state] = label.cost;")
        cpp = cpp.replace("std::tuple(this->search.cost[from], this->search.legs[from])", "std::tuple(this->SearchCost(from), this->SearchLegs(from))")
        cpp = cpp.replace("this->search.cost[from]", "this->SearchCost(from)")
        cpp = cpp.replace("this->search.cost[base]", "this->SearchCost(base)")
        cpp = cpp.replace("this->search.legs[from]", "this->SearchLegs(from)")
        cpp = cpp.replace("this->search.legs[base]", "this->SearchLegs(base)")
        cpp = cpp.replace("const int64_t source_cost = this->search.cost[this->expanding];", "const int64_t source_cost = this->SearchCost(this->expanding);")
        cpp = cpp.replace("const uint32_t source_legs = this->search.legs[this->expanding];", "const uint32_t source_legs = this->SearchLegs(this->expanding);")
        cpp = cpp.replace("\treturn used;\n}", "\tthis->diagnostic.bytes += this->search.generation.capacity() * sizeof(uint32_t);\n\treturn used;\n}")
    if contract:
        cpp = cpp.replace("if (this->optimized && !this->integrated) this->ContractGraph(nodes);", "if (this->optimized) this->ContractGraph(nodes);")
    if native_queue:
        cpp = re.sub(r'^\s*this->queue\.(SetOptimized|SetCapacityLimit|Reset)\([^\n]*\);\n', '', cpp, flags=re.M)
        cpp = cpp.replace("this->queue.AllocatedBytes()", "PassengerBenchmarkQueueBytes(this->queue)")
    cpp = cpp.replace('#include "passenger_precomputed.h"', f'#include "{name}.h"')
    cpp = re.sub(r'#include "([^"/]+)"', lambda m: m[0] if m[1] == name + '.h' else '#include "' + (source / "src" / m[1]).as_posix() + '"', cpp)
    cpp = cpp.replace("this->queue.emplace(", "this->MeasuredPush(")
    cpp = cpp.replace("auto [cost, legs, state] = this->queue.top(); this->queue.pop();", "auto [cost, legs, state] = this->MeasuredPop();")
    cpp = cpp.replace("\t\t\t++used;", "\t\t\t++used; this->metrics.peak_queue = std::max<uint64_t>(this->metrics.peak_queue, this->queue.size());")
    cpp = cpp.replace("Label next = ", "++this->metrics.copies; Label next = ")
    cpp = cpp.replace("next = this->SearchLabel(this->expanding);", "++this->metrics.copies; next = this->SearchLabel(this->expanding);")
    cpp = cpp.replace("this->SetSearchLabel(from, next);", "++this->metrics.successes; this->SetSearchLabel(from, next);")
    cpp = cpp.replace("assert(used > 0);", "if constexpr (PASSENGER_TABLE_COUNT) this->metrics.relaxations += used; assert(used > 0);")
    cpp = cpp.replace("\t\t\t\told = next;", "\t\t\t\t++this->metrics.successes; old = next;")
    cpp = cpp.replace("this->SetSearchLabel(edge.from, next);", "++this->metrics.successes; this->SetSearchLabel(edge.from, next);")
    for lookup in ("const auto &edge = this->edges[this->incoming[this->cursor++]];",
                   "const auto &edge = this->edges[this->optimized ? index : this->incoming[index]];",
                   "const auto &edge = this->integrated ? soa : this->edges[this->optimized ? index : this->incoming[index]];"):
        cpp = cpp.replace(lookup, "++this->metrics.relaxations; " + lookup)
    cpp = cpp.replace("\t\t\t\t\tconst auto &ride = this->section_rides[this->finishing", "\t\t\t\t\t++this->metrics.section_checks; const auto &ride = this->section_rides[this->finishing")
    cpp = cpp.replace("legs != this->scratch.labels[state].legs) continue;", "legs != this->scratch.labels[state].legs) { ++this->metrics.stale; continue; }")
    cpp = cpp.replace("legs != this->SearchLegs(state)) continue;", "legs != this->SearchLegs(state)) { ++this->metrics.stale; continue; }")
    cpp = cpp.replace("\tthis->scratch.labels.clear();", "\tthis->metrics.phase = this->metrics.row_started = Clock::now();\n\tthis->scratch.labels.clear();")
    if heap not in ("priority", "product"):
        cpp = cpp.replace("this->metrics.phase = this->metrics.row_started = Clock::now();\n\tthis->scratch.labels.clear();", "this->metrics.phase = this->metrics.row_started = Clock::now();\n\tthis->queue.reset(this->diagnostic.states);\n\tthis->scratch.labels.clear();")
    cpp = cpp.replace("else this->initializing_row = false;", "else { this->initializing_row = false; this->metrics.init_ns += Elapsed(this->metrics.phase); this->metrics.phase = Clock::now(); }")
    cpp = cpp.replace("this->finishing_row = true; continue;", "this->finishing_row = true; this->metrics.search_ns += Elapsed(this->metrics.phase); this->metrics.phase = Clock::now(); continue;")
    cpp = cpp.replace("\t\t\t\t\t/* Arrival/departure labels", "\t\t\t\t\tif (this->prepared.labels.empty()) { this->metrics.finish_ns += Elapsed(this->metrics.phase); this->metrics.phase = Clock::now(); }\n\t\t\t\t\t/* Arrival/departure labels")
    cpp = cpp.replace("\t\t\t\t\t++this->destination;", "\t\t\t\t\tthis->metrics.publish_ns += Elapsed(this->metrics.phase);\n\t\t\t\t\t++this->destination;")
    cpp = cpp.replace("\t\t\t\t\t++this->destination;", "\t\t\t\t\tthis->metrics.destinations_ns.push_back(Elapsed(this->metrics.row_started));\n\t\t\t\t\t++this->destination;")
    cpp = cpp.replace("\tstruct Path {", "\tconst auto section_started = Clock::now();\n\tstruct Path {")
    cpp = cpp.replace("success = true; co_return;", "this->metrics.sections_ns += Elapsed(section_started); success = true; co_return;")
    if simd and "/* Bounded waiting spans:" in cpp:
        cpp = '#include "' + (ROOT / "utils/passenger_table_build_simd.h").as_posix() + '"\n' + split_bounded_spans_simd(cpp)
    elif simd:
        cpp = '#include "' + (ROOT / "utils/passenger_table_build_simd.h").as_posix() + '"\n' + cpp
        begin = cpp.index("while (this->range_offset < count")
        begin = cpp.rfind("\n", 0, begin) + 1
        end = cpp.index("/* An excluded final state", begin)
        end = cpp.rfind("\n", 0, end) + 1
        accept = cpp.index("if (!materialized)", begin)
        accept = cpp.rfind("\n", 0, accept) + 1
        accept_end = cpp.index("this->MeasuredPush(cost, legs, from);", accept) + len("this->MeasuredPush(cost, legs, from);")
        accepted = textwrap.indent(textwrap.dedent(cpp[accept:accept_end]), "\t\t\t")
        loop = "\t\tauto accept = [&](uint32_t from) {\n" + accepted + "\n\t\t};\n" + '''
		while (this->range_offset < count && used < allowance) {
			if (finite && this->range_offset > 0 && count - this->range_offset >= 4 && allowance - used >= 4) {
				uint32_t from = range.begin + this->range_offset - 1;
				uint32_t mask = PassengerRangeMask(cost, legs, this->search.cost.data() + from, this->search.legs.data() + from);
				bool excluded = range.excluded >= from && range.excluded < from + 4;
				if (excluded) mask &= ~(1U << (range.excluded - from));
				this->range_offset += 4; used += 4 - excluded;
				while (mask != 0) { unsigned bit = std::countr_zero(mask); mask &= mask - 1; accept(from + bit); }
			} else {
				uint32_t from = this->range_offset == 0 ? this->hot_edges.from[index] : range.begin + this->range_offset - 1;
				++this->range_offset; if (from == range.excluded) continue; ++used;
				if (finite && std::tuple(cost, legs) < std::tuple(this->search.cost[from], this->search.legs[from])) accept(from);
			}
		}
'''
        cpp = cpp[:begin] + loop + cpp[end:]
    cpp = cpp.replace("\t\tthis->incoming_offsets.assign(nodes + 1, 0);", "\t\tconst auto csr_started = Clock::now();\n\t\tthis->incoming_offsets.assign(nodes + 1, 0);")
    cpp = cpp.replace("\t\tthis->rows.reserve(this->stations.size());", "\t\tthis->metrics.csr_ns += Elapsed(csr_started);\n\t\tthis->rows.reserve(this->stations.size());")
    if heap == "packed":
        cpp = cpp.replace("\t\tif (!this->stations.empty()) this->StartDestination();", "\t\tthis->queue.SetBounds(nodes, this->hot_edges.cost.empty() ? 0 : *std::max_element(this->hot_edges.cost.begin(), this->hot_edges.cost.end()));\n\t\tif (!this->stations.empty()) this->StartDestination();")
    # Count actual priority probes, independently of charged virtual Work.
    probe = "if (cost < this->search.cost[from] || (cost == this->search.cost[from] && legs < this->search.legs[from])) accept(from);"
    cpp = cpp.replace(probe, "++this->metrics.range_comparisons; " + probe)
    cpp = cpp.replace("\t\tif (state != this->exception_state[station] && this->SearchLabel(state) != base)", "\t\t++this->metrics.invariant_comparisons; if (state != this->exception_state[station] && this->SearchLabel(state) != base)")
    cpp = cpp.replace("uint32_t mask = PassengerRangeMask(", "if constexpr (PASSENGER_TABLE_COUNT) this->metrics.range_comparisons += 4; uint32_t mask = PassengerRangeMask(")
    cpp = re.sub(r'\+\+this->metrics\.\w+', lambda m: 'if constexpr (PASSENGER_TABLE_COUNT) ' + m[0], cpp)
    cpp = cpp.replace("this->metrics.peak_queue =", "if constexpr (PASSENGER_TABLE_COUNT) this->metrics.peak_queue =")
    (build / f"{name}.cpp").write_text(cpp, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("--source", type=pathlib.Path, default=ROOT)
    parser.add_argument("--candidate", default=None, help="Git ref; default: working tree")
    parser.add_argument("--candidate-snapshot", type=pathlib.Path, help="Frozen measurement-only h/cpp snapshot; requires --heap product")
    parser.add_argument("--reference", default=BASE, help="Pinned comparison ref; default: original baseline")
    parser.add_argument("--heap", choices=["priority", "indexed", "radix", "four", "packed", "product"], default="priority")
    parser.add_argument("--matrix", action="store_true", help="Compile all three queues over the same candidate graph")
    parser.add_argument("--timing-only", action="store_true", help="Remove diagnostic counter updates from timing runs")
    parser.add_argument("--simd", action="store_true", help="Isolated scalar/AVX2 ordered range experiment")
    parser.add_argument("--range-split", action="store_true", help="Compare scalar/SIMD before and after span splitting; requires an integrated --reference")
    parser.add_argument("--stages", action="store_true", help="Compile independent integration stages using the product queue")
    args = parser.parse_args()
    if args.candidate_snapshot and (args.candidate or args.heap != "product" or args.simd or args.matrix or args.stages or args.range_split):
        parser.error("--candidate-snapshot requires --heap product and excludes another candidate ref or graph/queue experiments")
    if args.range_split and args.reference == BASE:
        parser.error("--range-split requires an integrated --reference")
    if args.range_split and (args.heap != "product" or args.simd or args.matrix or args.stages):
        parser.error("--range-split requires --heap product and cannot combine with --simd, --matrix or --stages")
    build = args.build.resolve() / "table-build"
    build.mkdir(parents=True, exist_ok=True)
    generate(args.source.resolve(), build, "PassengerTableReference", args.reference, "product" if args.reference != BASE else "priority")
    generate(args.source.resolve(), build, "PassengerTableCandidate", args.candidate, args.heap, args.simd,
             snapshot=args.candidate_snapshot.resolve() if args.candidate_snapshot else None)
    sources = [build / "PassengerTableReference.cpp", build / "PassengerTableCandidate.cpp", ROOT / "utils/passenger_table_build_benchmark.cpp"]
    if args.range_split:
        for name, revision in [("PassengerTableBeforeSIMD", args.reference), ("PassengerTableSplitSIMD", args.candidate)]:
            generate(args.source.resolve(), build, name, revision, "product", simd=True)
            sources.append(build / f"{name}.cpp")
    if args.matrix:
        for suffix, heap in [("Priority", "priority"), ("Four", "four"), ("Packed", "packed"), ("Indexed", "indexed"), ("Radix", "radix")]:
            name = "PassengerTable" + suffix
            generate(args.source.resolve(), build, name, args.candidate, heap)
            sources.append(build / f"{name}.cpp")
        generate(args.source.resolve(), build, "PassengerTableGeneration", args.candidate, "product", generation=True)
        sources.append(build / "PassengerTableGeneration.cpp")
        generate(args.source.resolve(), build, "PassengerTablePlain", args.candidate, "product")
        sources.append(build / "PassengerTablePlain.cpp")
    if args.stages:
        for suffix, revision in [("HotEdge", "4d98df0ac6"), ("Ranges", "5fb8a07385"), ("Init", "3601641832"), ("Batch", "7abdbaa1b4")]:
            name = "PassengerTable" + suffix
            generate(args.source.resolve(), build, name, revision, "product")
            sources.append(build / f"{name}.cpp")
        generate(args.source.resolve(), build, "PassengerTableContract", args.candidate, "product", contract=True)
        sources.append(build / "PassengerTableContract.cpp")
    cmake = 'function(add_table_build_benchmark)\n  target_sources(openttd_test PRIVATE\n' + ''.join(f'    "{p.as_posix()}"\n' for p in sources) + ')\n  target_include_directories(openttd_test PRIVATE "' + build.as_posix() + '")\nendfunction()\ncmake_language(DEFER CALL add_table_build_benchmark)\n'
    if args.matrix:
        cmake = cmake.replace("endfunction()", "  target_compile_definitions(openttd_test PRIVATE WITH_TABLE_BUILD_HEAP_MATRIX)\nendfunction()")
    if args.stages:
        cmake = cmake.replace("endfunction()", "  target_compile_definitions(openttd_test PRIVATE WITH_TABLE_BUILD_STAGES)\nendfunction()")
    if args.range_split:
        cmake = cmake.replace("endfunction()", "  target_compile_definitions(openttd_test PRIVATE WITH_TABLE_BUILD_RANGE_SPLIT)\nendfunction()")
    if args.timing_only:
        definitions = "  set_source_files_properties(\n" + ''.join(f'    "{p.as_posix()}"\n' for p in sources)
        definitions += "    PROPERTIES COMPILE_DEFINITIONS PASSENGER_TABLE_COUNT=0)\n"
        cmake = cmake.replace("endfunction()", definitions + "endfunction()")
    (build / "benchmark.cmake").write_text(cmake, encoding="utf-8")
    manifest = dict(reference=args.reference, candidate=args.candidate, candidate_snapshot=str(args.candidate_snapshot.resolve()) if args.candidate_snapshot else None, heap=args.heap, timing_only=args.timing_only,
                    source_head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=args.source).decode().strip(),
                    simd=args.simd, stages=args.stages, matrix=args.matrix,
                    range_split=args.range_split,
                    tools={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in [ROOT / "utils" / n for n in ("passenger_table_build_prepare.py", "passenger_table_build_benchmark.cpp", "passenger_table_build_heaps.h", "passenger_table_build_simd.h")]},
                    files={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in build.glob("PassengerTable*.*")})
    (build / "source.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(build / "benchmark.cmake")


if __name__ == "__main__":
    main()
