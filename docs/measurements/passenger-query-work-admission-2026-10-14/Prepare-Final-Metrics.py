"""Read-only CPU, Packet peak and ordered query/result probes in copied TUs only."""
import argparse
import hashlib
import json
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one measurement anchor: {old[:90]}")
    return text.replace(old, new)


def attach(text, header):
    return once(text, '#include "' + (ROOT / 'src/safeguards.h').as_posix() + '"',
                '#include "' + header.as_posix() + '"\n#include "' + (ROOT / 'src/safeguards.h').as_posix() + '"')


def absolute(text):
    return re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (ROOT / 'src' / m[1]).as_posix() + '"'
                  if (ROOT / 'src' / m[1]).is_file() else m[0], text)


def prepare(build):
    copied = build / 'journey-profile'
    header = copied / 'final_metrics.h'
    header.write_text(r'''#pragma once
#include <windows.h>
namespace FinalMetrics {
inline bool enabled = false;
inline uint64_t cpu_100ns = 0, cpu_cycles = 0, cpu_calls = 0, cpu_failures = 0, packet_peak = 0;
inline uint64_t query_digest = 14695981039346656037ULL, result_digest = 14695981039346656037ULL, admission_digest = 14695981039346656037ULL;
inline uint64_t query_count = 0, result_count = 0, admission_count = 0;
inline uint64_t peak_tick_queries = 0, peak_tick_work = 0, old_accepted = 0, old_query_rejected = 0, old_work_rejected = 0;
inline void Tick(uint64_t queries, uint64_t work) { if (enabled) { peak_tick_queries = std::max(peak_tick_queries, queries); peak_tick_work = std::max(peak_tick_work, work); } }
template<class T> uint64_t Number(T v) {
    if constexpr (requires { v.base(); }) return uint64_t(v.base());
    else if constexpr (std::is_enum_v<T>) return uint64_t(to_underlying(v));
    else return uint64_t(v);
}
template<class T> void Mix(uint64_t &digest, T v) {
    uint64_t n = Number(v);
    for (int i = 0; i < 8; ++i) { digest ^= n & 255; digest *= 1099511628211ULL; n >>= 8; }
}
template<class... T> void Query(T... v) {
    if (!enabled) return;
    ++query_count; (Mix(query_digest, v), ...);
}
template<class... T> void Admission(T... v) {
    if (!enabled) return;
    ++admission_count; (Mix(admission_digest, v), ...);
}
template<class R> void Ride(const R &r) {
    Mix(result_digest, r.vehicle); Mix(result_digest, r.service); Mix(result_digest, r.boarding); Mix(result_digest, r.alighting);
    Mix(result_digest, r.boarding_order); Mix(result_digest, r.alighting_order); Mix(result_digest, r.departure); Mix(result_digest, r.arrival);
    Mix(result_digest, r.signature); Mix(result_digest, r.kind); Mix(result_digest, r.generation); Mix(result_digest, r.not_before);
    Mix(result_digest, r.planned_at); Mix(result_digest, r.estimated_wait); Mix(result_digest, r.waiting_limit); Mix(result_digest, r.timed_deferred);
}
template<class P> void Result(const P &p) {
    if (!enabled) return;
    ++result_count; Mix(result_digest, p.state); Mix(result_digest, p.reason); Mix(result_digest, p.work); Mix(result_digest, p.deferred);
    Mix(result_digest, p.cost); Mix(result_digest, p.arrival); Mix(result_digest, p.transfers); Mix(result_digest, p.ride.has_value());
    if (p.ride) Ride(*p.ride);
    Mix(result_digest, p.route.size()); for (const auto &ride : p.route) Ride(ride);
}
inline void Packet() { if (enabled) packet_peak = std::max<uint64_t>(packet_peak, CargoPacket::GetNumItems()); }
inline void Reset() {
    enabled = true; cpu_100ns = cpu_cycles = cpu_calls = cpu_failures = query_count = result_count = admission_count = 0;
    query_digest = result_digest = admission_digest = 14695981039346656037ULL;
    peak_tick_queries = peak_tick_work = old_accepted = old_query_rejected = old_work_rejected = 0;
    packet_peak = CargoPacket::GetNumItems();
}
struct CpuScope {
    uint64_t start = 0, cycles = 0;
    bool observing = false;
    static bool Read(uint64_t &time, uint64_t &cycles) {
        FILETIME created, exited, kernel, user; ULONG64 c;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) || !QueryThreadCycleTime(GetCurrentThread(), &c)) return false;
        time = ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) + ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
        cycles = c; return true;
    }
    CpuScope() : observing(enabled) { if (observing) { ++cpu_calls; if (!Read(start, cycles)) { ++cpu_failures; observing = false; } } }
    ~CpuScope() { if (observing) { uint64_t end, c; if (Read(end, c)) { cpu_100ns += end - start; cpu_cycles += c - cycles; } else ++cpu_failures; } }
};
}
''', encoding='utf-8')
    console_path = copied / 'console_cmds.cpp'
    console = attach(console_path.read_text(encoding='utf-8'), header)
    console = once(console, 'if (operation == "reset") { ResetPassengerJourneyDiagnostic(); return true; }',
                   'if (operation == "reset") { ResetPassengerJourneyDiagnostic(); FinalMetrics::Reset(); return true; }')
    anchor = '\tconst auto stats = operation == "tick" ? GetPassengerJourneyTickDiagnostic() : GetPassengerJourneyTotalDiagnostic();'
    console = once(console, anchor, anchor + '\n\tIConsolePrint(CC_DEFAULT, "Final measurement: timed_cpu_100ns={}; timed_cpu_cycles={}; timed_calls={}; cpu_failures={}; packet_current={}; packet_peak={}; query_count={}; query_digest={}; result_count={}; result_digest={}; admission_count={}; admission_digest={}",\n'
        '\t\tFinalMetrics::cpu_100ns, FinalMetrics::cpu_cycles, FinalMetrics::cpu_calls, FinalMetrics::cpu_failures, CargoPacket::GetNumItems(), FinalMetrics::packet_peak, FinalMetrics::query_count, FinalMetrics::query_digest, FinalMetrics::result_count, FinalMetrics::result_digest, FinalMetrics::admission_count, FinalMetrics::admission_digest);\n'
        '\tIConsolePrint(CC_DEFAULT, "Final old admission: accepted={}; rejected_queries={}; rejected_work={}; peak_tick_queries={}; peak_tick_work={}", FinalMetrics::old_accepted, FinalMetrics::old_query_rejected, FinalMetrics::old_work_rejected, FinalMetrics::peak_tick_queries, FinalMetrics::peak_tick_work);')
    console_path.write_text(console, encoding='utf-8')
    db = absolute((ROOT / 'src/passenger_service_database.cpp').read_text(encoding='utf-8'))
    db = attach(db, header)
    old = '\treturn this->PlanInternal(cargo, origin, destination, ready, limits, evaluation, excluded, true);'
    db = once(db, old, '\tFinalMetrics::CpuScope cpu;\n\tFinalMetrics::Query(_state_ticks, cargo, origin, destination, ready, limits.work, excluded);\n'
                   '\tauto result = this->PlanInternal(cargo, origin, destination, ready, limits, evaluation, excluded, true);\n'
                   '\tFinalMetrics::Result(result);\n\treturn result;')
    packet = absolute((ROOT / 'src/cargopacket.cpp').read_text(encoding='utf-8'))
    packet = attach(packet, header)
    constructors = [m.start() for m in re.finditer(r'CargoPacket::CargoPacket\(', packet)]
    for start in reversed(constructors):
        brace = packet.index('{', start) + 1
        packet = packet[:brace] + '\n\tFinalMetrics::Packet();' + packet[brace:]
    if len(constructors) != 4:
        raise ValueError('CargoPacket constructor inventory changed')
    project = build / 'openttd_lib.vcxproj'
    xml = project.read_text(encoding='utf-8-sig')
    for name, text in [('passenger_service_database.cpp', db), ('cargopacket.cpp', packet)]:
        target = copied / name
        target.write_text(text, encoding='utf-8')
        xml = once(xml, str(ROOT / 'src' / name), str(target))
    project.write_text(xml, encoding='utf-8-sig')
    metadata_path = copied / 'instrumentation.json'
    metadata = json.loads(metadata_path.read_text(encoding='utf-8'))
    metadata['final_metrics'] = {'timed_cpu': 'GetThreadTimes user+kernel 100ns units; coarse per-call OS accounting; QueryThreadCycleTime cycles also retained',
        'packet_peak': 'every CargoPacket constructor after reset', 'ordered_digests': 'FNV-1a of explicit numeric fields, not raw struct bytes; full-save SHA256 also required',
        'header_sha256': hashlib.sha256(header.read_bytes()).hexdigest(), 'script_sha256': hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()}
    metadata_path.write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('build', type=pathlib.Path)
    prepare(p.parse_args().build.resolve())
