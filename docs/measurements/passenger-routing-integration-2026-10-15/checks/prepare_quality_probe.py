import hashlib
import json
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
build = root / 'build-integration'
for tool, flags in [('passenger_precomputed_join_prepare.py', ['a2915e1844']),
                    ('passenger_precomputed_real_map.py', ['driver'])]:
    command = [sys.executable, '-B', str(root / 'utils' / tool)]
    command += [*flags, str(root), str(build), 'a2915e1844'] if flags == ['driver'] else [str(root), str(build), *flags]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
console = build / 'save-integrity/console_cmds.cpp'
content = console.read_text(encoding='utf-8')
anchor = '    if (operation == "partial_precomputed") {'
assert content.count(anchor) == 1
code = '''    if (operation == "quality_probe") {
        auto &db = GetPassengerServiceDatabase();
        for (const char *id : {"1330", "3716", "3671", "5131"}) {
            std::array<std::string_view, 2> command{"passenger_service_inspect", id};
            ConPassengerServiceInspect(command);
            const Vehicle *v = Vehicle::GetIfValid(VehicleID{ParseInteger<uint32_t>(id).value()});
            const auto *s = v != nullptr && v->orders != nullptr ? db.Get(v->orders->index) : nullptr;
            if (s != nullptr) {
                size_t phases = 0; for (const auto &vehicle : s->vehicles) phases += vehicle.phase_known;
                IConsolePrint(CC_DEFAULT, "Quality published vehicle={}; service={}; period={}; stops={}; vehicles={}; known_phases={}", id, s->id, s->timetable.period, s->timetable.stops.size(), s->vehicles.size(), phases);
            } else IConsolePrint(CC_DEFAULT, "Quality published vehicle={}; absent=true", id);
        }
        for (auto [origin, destination] : {std::pair{3221, 400}, std::pair{582, 3773}, std::pair{4159, 4158}, std::pair{3393, 3394}}) {
            const auto p = db.PlanPrecomputed(CargoType{0}, StationID{uint16_t(origin)}, StationID{uint16_t(destination)}, _state_ticks);
            if (p.ride) IConsolePrint(CC_DEFAULT, "Quality plan {}->{}: state={}; service={}; alight={}; wait={}; cost={}; riding={}; total_wait={}; transfers={}", origin, destination, to_underlying(p.state), p.ride->service, p.ride->alighting, p.ride->estimated_wait, p.cost, p.riding, p.waiting, p.transfers);
            else IConsolePrint(CC_DEFAULT, "Quality plan {}->{}: state={}; no_ride=true", origin, destination, to_underlying(p.state));
        }
        return true;
    }
'''
console.write_text(content.replace(anchor, code + anchor), encoding='utf-8')
metadata = dict(runtime_revision='a2915e1844', purpose='Read-only service timetable and initial OD-plan diagnostics; never used for performance',
                console_sha256=hashlib.sha256(console.read_bytes()).hexdigest(), inserted_code=code)
(build / 'quality-probe-instrumentation.json').write_text(json.dumps(metadata, indent=2) + '\n')
print('Read-only quality probe prepared; build only after profile runs finish')
