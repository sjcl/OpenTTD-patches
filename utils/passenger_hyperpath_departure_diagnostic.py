#!/usr/bin/env python3
"""Add the opt-in 3393/3394 departure observer to copied measurement TUs only."""
import re
from pathlib import Path


def once(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f"Departure diagnostic anchor changed: {before[:100]}")
    return text.replace(before, after)


def function(text, signature, transform):
    if text.count(signature) != 1:
        raise ValueError(f"Expected one definition: {signature}")
    begin = text.index(signature)
    end = text.index('\n}\n', begin) + 2
    return text[:begin] + transform(text[begin:end]) + text[end:]


def prepare(source, build):
    source, build = source.resolve(), build.resolve()
    if 'MayBoardHyperpath' not in (source / 'src/passenger_service_database.h').read_text(encoding='utf-8'):
        raise ValueError('Departure diagnostics require the separated membership API')
    folder = build / 'target-departure'
    folder.mkdir(exist_ok=True)
    original = source / 'utils/passenger_hyperpath_departure_diagnostic.h'
    header = folder / 'diagnostic.h'
    content = original.read_text(encoding='utf-8')
    content = re.sub(r'#include "([^"]+)"', lambda m: f'#include "{(original.parent / m[1]).resolve().as_posix()}"', content)
    header.write_text(content, encoding='utf-8')
    project = build / 'openttd_lib.vcxproj'
    xml = project.read_text(encoding='utf-8-sig')

    def instrument(name, transform):
        nonlocal xml
        matches = [Path(path) for path in re.findall(r'<ClCompile Include="([^"]+)"', xml) if Path(path).name == name]
        if len(matches) != 1:
            raise ValueError(f'Expected one compiled {name}')
        active = matches[0]
        text = active.read_text(encoding='utf-8')
        if active.is_relative_to(source / 'src'):
            text = re.sub(r'#include "([^"]+)"', lambda m:
                f'#include "{(active.parent / m[1]).resolve().as_posix()}"' if (active.parent / m[1]).is_file() else m[0], text)
            target = folder / name
            xml = once(xml, str(active), str(target))
        else:
            target = active
        anchor = f'#include "{(source / "src/stdafx.h").as_posix()}"\n'
        text = once(text, anchor, anchor + f'#include "{header.as_posix()}"\n')
        target.write_text(transform(text), encoding='utf-8')

    def trace_hooks(text):
        matches = list(re.finditer(r'PassengerTransportTrace::(Birth|Erase|Split|Merge|Reduce|Board)\(', text))
        for match in reversed(matches):
            depth, end = 1, match.end()
            while depth:
                depth += (text[end] == '(') - (text[end] == ')')
                end += 1
            if text[end:end + 1] != ';':
                raise ValueError('Expected a void observer statement')
            call = text[match.start():end + 1]
            text = text[:end + 1] + ' ' + call.replace('PassengerTransportTrace::', 'PassengerDepartureDiagnostic::') + text[end + 1:]
        return text

    def cargo(text):
        text = trace_hooks(text)
        lookup = '''
    if (const auto *list = PassengerDepartureDiagnostic::index_query) {
        auto found = list->waiting_entries.find(PassengerDepartureDiagnostic::index_packet);
        PassengerDepartureDiagnostic::indexed = list->waiting_index_valid && found != list->waiting_entries.end();
        PassengerDepartureDiagnostic::pending = PassengerDepartureDiagnostic::indexed && found->second.pending;
        PassengerDepartureDiagnostic::indexed_due = PassengerDepartureDiagnostic::indexed ? found->second.routing_due : INVALID_STATE_TICKS;
        PassengerDepartureDiagnostic::generation_pending = PassengerDepartureDiagnostic::indexed && found->second.routing_due == INVALID_STATE_TICKS &&
            (list->passenger_routing_dirty || list->passenger_routing_sweeping) && list->NextPassengerGenerationPacket(PassengerDepartureDiagnostic::index_packet.base()) == PassengerDepartureDiagnostic::index_packet &&
            (list->passenger_routing_dirty || (PassengerDepartureDiagnostic::index_packet.base() >= list->passenger_routing_cursor && PassengerDepartureDiagnostic::index_packet.base() <= list->passenger_routing_end));
        if (PassengerDepartureDiagnostic::generation_pending) PassengerDepartureDiagnostic::indexed_due = list->passenger_routing_due;
        PassengerDepartureDiagnostic::indexed_key = PassengerDepartureDiagnostic::indexed ? found->second.key : StationID::Invalid();
        PassengerDepartureDiagnostic::indexed_sequence = PassengerDepartureDiagnostic::indexed ? found->second.sequence : 0;
        return _passenger_event_diagnostic;
    }
'''
        text = once(text, 'PassengerWaitingEventDiagnostic GetPassengerWaitingEventDiagnostic()\n{', 'PassengerWaitingEventDiagnostic GetPassengerWaitingEventDiagnostic()\n{' + lookup)
        for name, argument in [('SetPassengerJourney', '&journey'), ('FallbackPassengerJourney', 'nullptr'), ('DeferPassengerJourney', 'nullptr')]:
            text = function(text, f'void CargoPacket::{name}(', lambda body, arg=argument: once(body, '\n{', '\n{\n\tPassengerDepartureDiagnostic::Decision(*this, ' + arg + ');'))
        return text

    instrument('cargopacket.cpp', cargo)
    instrument('cargoaction.cpp', trace_hooks)
    instrument('economy.cpp', lambda text: function(text, 'static void LoadUnloadVehicle(Vehicle *front, PassengerJourneyLoadingContext &loading)',
        lambda body: once(body, '\n{', '\n{\n\tPassengerDepartureDiagnostic::BeforeLoad(*front);')))
    def vehicle(text):
        return function(text, 'void Vehicle::LeaveStation()', lambda body: once(once(body,
            '\tthis->current_order.MakeLeaveStation();', '\tPassengerDepartureDiagnostic::BeforeDeparture(*this);\n\tthis->current_order.MakeLeaveStation();'),
            '\tthis->CancelReservation(StationID::Invalid(), st);', '\tthis->CancelReservation(StationID::Invalid(), st);\n\tPassengerDepartureDiagnostic::AfterCancel(*this);'))
    instrument('vehicle.cpp', vehicle)
    def orders(text):
        text = function(text, 'bool ProcessOrders(Vehicle *v)', lambda body:
            once(body, '\n{', '\n{\n\tPassengerDepartureDiagnostic::OrderScope diagnostic_order(v);'))
        return function(text, 'bool UpdateOrderDest(Vehicle *v, const Order *order, int conditional_depth, bool pbs_look_ahead)', lambda body:
            once(body, '\n{', '\n{\n\tPassengerDepartureDiagnostic::ExecuteOrder(v, order, conditional_depth, pbs_look_ahead);'))
    instrument('order_cmd.cpp', orders)
    instrument('console_cmds.cpp', lambda text: once(text, 'ResetPassengerJourneyDiagnostic(); return true;',
        'PassengerDepartureDiagnostic::Reset(); ResetPassengerJourneyDiagnostic(); return true;'))
    target = build / 'real-map-driver/null_v.cpp'
    text = target.read_text(encoding='utf-8')
    anchor = f'#include "{(source / "src/stdafx.h").as_posix()}"\n'
    text = once(text, anchor, anchor + f'#include "{header.as_posix()}"\n')
    text = once(text, 'std::ofstream out("actual-ticks.json");', 'PassengerDepartureDiagnostic::Dump();\n        std::ofstream out("actual-ticks.json");')
    target.write_text(text, encoding='utf-8')
    project.write_text(xml, encoding='utf-8-sig')
