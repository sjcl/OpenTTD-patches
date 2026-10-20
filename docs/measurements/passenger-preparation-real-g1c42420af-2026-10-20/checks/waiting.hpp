/* Opt-in observations in copied TUs only. No admission, Work or cadence changes. */
#pragma once
#include "passenger_service_database.h"
#include "console_func.h"
#include "date_func.h"
#include "openttd.h"
#include "settings_type.h"
#include "cargotype.h"
#include <windows.h>
#include <tlhelp32.h>
#include <fstream>
#include <chrono>
#include <array>
namespace WaitingProbe {
using Clock = std::chrono::steady_clock;
inline bool Enabled() { return getenv("OPENTTD_WAITING_ROLE") != nullptr; }
inline bool Client() { const char *r = getenv("OPENTTD_WAITING_ROLE"); return r != nullptr && std::string_view(r) == "client"; }
inline uint64_t Cpu(HANDLE handle, bool thread)
{
    FILETIME c{}, e{}, k{}, u{};
    if (!(thread ? GetThreadTimes(handle, &c, &e, &k, &u) : GetProcessTimes(handle, &c, &e, &k, &u))) return 0;
    return ((uint64_t(k.dwHighDateTime) << 32) + k.dwLowDateTime + (uint64_t(u.dwHighDateTime) << 32) + u.dwLowDateTime) * 100;
}
inline uint64_t WorkerCpu()
{
    uint64_t total = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 e{}; e.dwSize = sizeof(e);
    if (Thread32First(snapshot, &e)) do {
        if (e.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, false, e.th32ThreadID);
        if (thread == nullptr) continue;
        PWSTR name = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &name)) && name != nullptr) {
            if (std::wstring_view(name) == L"pax-destination") total += Cpu(thread, true);
            LocalFree(name);
        }
        CloseHandle(thread);
    } while (Thread32Next(snapshot, &e));
    CloseHandle(snapshot); return total;
}
inline uint64_t Ns(Clock::duration duration) { return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count(); }
inline bool armed = false, started = false, completed = false;
inline uint32_t depth = 0;
inline uint64_t start_process_cpu = 0, start_main_cpu = 0, start_worker_cpu = 0, slices = 0, max_slice = 0;
inline int64_t start_tick = 0;
inline std::array<uint64_t, 8> phase_cpu_wall{}, phase_wall{};
inline Clock::time_point first{}, last_end{}, local_ready{};
inline size_t last_phase = 0;
inline void Arm()
{
    if (!Enabled() || armed) return;
    armed = true; start_tick = _state_ticks.base();
    std::ofstream("waiting-armed.json") << "{\"tick\":" << start_tick << ",\"pid\":" << GetCurrentProcessId() << "}\n";
}
struct Scope {
    bool entered = false, outer = false;
    size_t phase = 0;
    Clock::time_point begin{};
    PassengerServiceDatabase &db;
    Scope(PassengerServiceDatabase &database) : db(database)
    {
        if (!Enabled() || completed || !db.IsPreparingPrecomputed()) return;
        if (!armed && Client()) Arm();
        if (!armed) return;
        entered = true; outer = depth++ == 0;
        if (!outer) return;
        if (!started) {
            start_worker_cpu = WorkerCpu(); start_main_cpu = Cpu(GetCurrentThread(), true); start_process_cpu = Cpu(GetCurrentProcess(), false);
            first = Clock::now(); started = true;
        }
        phase = static_cast<size_t>(db.GetPrecomputedProgress().phase); begin = Clock::now();
        if (last_end != Clock::time_point{}) phase_wall[last_phase] += Ns(begin - last_end);
    }
    ~Scope()
    {
        if (!entered) return;
        --depth;
        if (!outer) return;
        const auto end = Clock::now(); const auto elapsed = Ns(end - begin);
        phase_cpu_wall[phase] += elapsed; phase_wall[phase] += elapsed; last_phase = phase; last_end = end;
        ++slices; max_slice = std::max(max_slice, elapsed);
        if (!db.IsPreparingPrecomputed() && local_ready == Clock::time_point{}) local_ready = end;
    }
};
inline bool Complete(const char *kind)
{
    if (!Enabled() || completed || (Client() != (std::string_view(kind) == "multiplayer"))) return false;
    Arm();
    auto &db = GetPassengerServiceDatabase();
    if (db.IsPreparingPrecomputed() || db.GetLoadState() != PassengerServiceDatabase::LoadState::Ready) return false;
    const auto end = Clock::now();
    const auto main_cpu = Cpu(GetCurrentThread(), true), process_cpu = Cpu(GetCurrentProcess(), false);
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user); GetSystemTimeAsFileTime(&now);
    auto value = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) + f.dwLowDateTime; };
    const auto process_to_ready = (value(now) - value(created)) * 100;
    const auto workers = WorkerCpu();
    if (started && last_end != Clock::time_point{}) phase_wall[last_phase] += Ns(end - last_end);
    std::ofstream out("waiting-ready.json");
    out << "{\"kind\":\"" << kind << "\",\"ready\":true,\"tick_before\":" << start_tick << ",\"tick_after\":" << _state_ticks.base()
        << ",\"process_to_ready_ns\":" << process_to_ready << ",\"preparation_wall_ns\":" << (started ? Ns(end - first) : 0)
        << ",\"preparation_local_ready_ns\":" << (started && local_ready != Clock::time_point{} ? Ns(local_ready - first) : 0)
        << ",\"main_cpu_ns\":" << (started ? main_cpu - start_main_cpu : 0)
        << ",\"process_cpu_ns\":" << (started ? process_cpu - start_process_cpu : 0)
        << ",\"worker_cpu_ns\":" << (started ? workers - start_worker_cpu : 0)
        << ",\"slices\":" << slices << ",\"max_slice_ns\":" << max_slice << ",\"load_work\":" << db.GetLoadWork()
        << ",\"requested_workers\":" << uint(_settings_client.gui.passenger_precomputed_workers) << ",\"phase_call_ns\":[";
    for (size_t i = 0; i < 8; ++i) out << (i ? "," : "") << phase_cpu_wall[i];
    out << "],\"phase_wall_ns\":[";
    for (size_t i = 0; i < 8; ++i) out << (i ? "," : "") << phase_wall[i];
    out << "],\"tables\":["; bool comma = false;
    for (CargoType cargo : EnumRange(NUM_CARGO)) if (IsCargoInClass(cargo, CargoClass::Passengers)) {
        const auto d = db.GetPrecomputedDiagnostic(cargo); const auto p = db.GetPrecomputedParallelDiagnostic(cargo);
        out << (comma ? "," : "") << "{\"cargo\":" << uint(cargo) << ",\"rows\":" << d.destinations << ",\"retained_rows\":" << d.retained_destinations
            << ",\"work\":" << d.operations << ",\"workers\":" << p.workers << ",\"hits\":" << p.cache_hits << ",\"serial_rows\":" << p.serial_rows
            << ",\"fallbacks\":" << p.fallbacks << ",\"finished_rows\":" << p.finished_rows << "}"; comma = true;
    }
    out << "]}\n"; out.close(); completed = true;
    IConsoleCmdExec("passenger_journey_stats save_integrity");
    if (!Client()) _exit_game = true;
    return true;
}
}
