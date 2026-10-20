/* Copied console TU only. Public API calls and OS observations, no routing changes. */
#include <windows.h>
#include <tlhelp32.h>
namespace {
uint64_t PreparationWorkerCpu()
{
    uint64_t total = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) do {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, false, entry.th32ThreadID);
        if (thread == nullptr) continue;
        PWSTR name = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &name)) && name != nullptr) {
            if (std::wstring_view(name) == L"pax-destination") {
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetThreadTimes(thread, &created, &exited, &kernel, &user))
                    total += ((uint64_t(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
                        (uint64_t(user.dwHighDateTime) << 32) + user.dwLowDateTime) * 100;
            }
            LocalFree(name);
        }
        CloseHandle(thread);
    } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    return total;
}
uint64_t PreparationCpu(bool thread)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = thread ? GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) :
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    if (!ok) return 0;
    return ((uint64_t(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
        (uint64_t(user.dwHighDateTime) << 32) + user.dwLowDateTime) * 100;
}
bool MeasurePassengerPreparation(bool replay)
{
    auto &db = GetPassengerServiceDatabase();
    const auto worker_cpu = PreparationWorkerCpu();
    const auto process = PreparationCpu(false), main = PreparationCpu(true);
    const auto start = std::chrono::steady_clock::now();
    const int64_t tick = _state_ticks.base();
    std::array<uint64_t, 8> phases{};
    size_t slices = 0;
    uint64_t max_slice = 0;
    while (replay ? db.GetLoadState() == PassengerServiceDatabase::LoadState::Replaying : db.IsPreparingPrecomputed()) {
        const auto phase = static_cast<size_t>(db.GetPrecomputedProgress().phase);
        const auto before = std::chrono::steady_clock::now();
        if (replay) db.StepLoad(); else db.PreparePrecomputed();
        const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - before).count();
        phases[phase] += ns; max_slice = std::max(max_slice, ns); ++slices;
        if (std::chrono::steady_clock::now() - start > std::chrono::minutes(10)) return false;
    }
    const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    const auto process_end = PreparationCpu(false), main_end = PreparationCpu(true);
    const auto worker_end = PreparationWorkerCpu();
    IConsolePrint(CC_DEFAULT, "Preparation probe: kind={}; ready={}; tick_before={}; tick_after={}; wall_ns={}; process_cpu_ns={}; main_cpu_ns={}; slices={}; max_slice_ns={}; services_ns={}; graph_ns={}; retained_ns={}; tables_ns={}; load_work={}; requested_workers={}; worker_cpu_ns={}",
        replay ? "replay" : "initial", db.GetLoadState() == PassengerServiceDatabase::LoadState::Ready && (!replay ? !db.IsPreparingPrecomputed() : true),
        tick, _state_ticks.base(), wall, process_end - process, main_end - main, slices, max_slice,
        phases[1], phases[2], phases[3], phases[4], db.GetLoadWork(), _settings_client.gui.passenger_precomputed_workers, worker_end - worker_cpu);
    for (CargoType cargo : EnumRange(NUM_CARGO)) if (IsCargoInClass(cargo, CargoClass::Passengers)) {
        const auto d = db.GetPrecomputedDiagnostic(cargo);
        const auto p = db.GetPrecomputedParallelDiagnostic(cargo);
        IConsolePrint(CC_DEFAULT, "Preparation rows: cargo={}; rows={}; retained_rows={}; work={}; workers={}; searches={}; search_ns={}; cache_hits={}; serial_rows={}; fallbacks={}; finished_rows={}; peak_pending={}",
            cargo, d.destinations, d.retained_destinations, d.operations, p.workers, p.searches, p.search_nanoseconds,
            p.cache_hits, p.serial_rows, p.fallbacks, p.finished_rows, p.peak_pending);
    }
    return tick == _state_ticks.base();
}
bool WaitPassengerMeasurementBackground()
{
    const auto started = std::chrono::steady_clock::now();
    const auto tick = _state_ticks;
    size_t total = 0; for (const LinkGraphJob *job : LinkGraphJob::Iterate()) ++total;
    for (;;) {
        bool complete = true;
        for (const LinkGraphJob *job : LinkGraphJob::Iterate()) complete &= job->IsJobCompleted();
        if (complete) break;
        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(10)) return false;
        CSleep(10);
    }
    IConsolePrint(CC_DEFAULT, "Preparation background wait: complete=true; jobs={}; tick_before={}; tick_after={}; elapsed_us={}",
        total, tick.base(), _state_ticks.base(), std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    return tick == _state_ticks;
}
bool MakePassengerReplayFixture()
{
    auto &db = GetPassengerServiceDatabase();
    if (db.GetLoadState() != PassengerServiceDatabase::LoadState::Ready || !db.PrecomputedFinished()) return false;
    const int64_t tick = _state_ticks.base();
    IConsoleCmdExec("setting linkgraph.passenger_transfer_penalty 17");
    while (db.IsPreparingPrecomputed()) db.PreparePrecomputed();
    auto count = [&]() { size_t rows = 0; for (CargoType cargo : EnumRange(NUM_CARGO)) rows += db.GetPrecomputedDiagnostic(cargo).destinations; return rows; };
    for (size_t loops = 0; count() == 0; ++loops) {
        if (loops == 100000000) return false;
        db.TickPrecomputed(1);
    }
    db.TickPrecomputed(7);
    IConsolePrint(CC_DEFAULT, "Mixed replay fixture: ready={}; completed_rows={}; tick_before={}; tick_after={}",
        !db.PrecomputedFinished(), count(), tick, _state_ticks.base());
    return !db.PrecomputedFinished() && tick == _state_ticks.base();
}
}
