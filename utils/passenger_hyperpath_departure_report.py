#!/usr/bin/env python3
"""Summarize opt-in departure observations; counts are passenger opportunities."""
import argparse
import collections
import csv
import json
from pathlib import Path


def rows(directory, name):
    path = directory / name
    if not path.is_file():
        return []
    with path.open(encoding='utf-8', newline='') as stream:
        return [{key: int(value) if value.lstrip('-').isdigit() else value for key, value in row.items()} for row in csv.DictReader(stream)]


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return dict(count=len(values), minimum=values[0], maximum=values[-1], mean=sum(values) / len(values))


def summarize(directory):
    process = json.loads((directory / 'process.json').read_text(encoding='utf-8'))
    if process['exit_code'] or process['contamination'] or not process.get('departure_diagnostic'):
        raise ValueError('Expected a successful, exclusive departure-diagnostic run')
    departures = rows(directory, 'target-departures.csv')
    waiting = rows(directory, 'target-waiting.csv')
    reserved = rows(directory, 'target-reservations.csv')
    checks = rows(directory, 'target-readonly-checks.csv')
    before = [row for row in departures if row['phase'] == 'before_cancel']
    services = {}
    modes = ('current_hyperpath', 'other_intent', 'deferred', 'fallback', 'legacy')
    flags = ('early', 'expired', 'pending', 'generation_pending')
    for service in (524, 525, 528):
        stops = [row for row in before if row['service'] == service]
        packets = [row for row in waiting if row['service'] == service]
        virtual = [row for row in reserved if row['service'] == service]
        after = [row for row in departures if row['service'] == service and row['phase'] == 'after_cancel']
        states = {mode: sum(row['units'] for row in packets if row['mode'] == index) for index, mode in enumerate(modes)}
        states.update({flag: sum(row['units'] for row in packets if row[flag]) for flag in flags})
        states['unindexed'] = sum(row['units'] for row in packets if not row['indexed'])
        states['new_unprocessed'] = sum(row['new_unprocessed_units'] for row in packets)
        states['unknown_event_age'] = sum(row['units'] for row in packets if row['oldest_event_ticks'] < 0)
        membership = {str(value): sum(row['units'] for row in packets if row['member'] == value) for value in (-1, 0, 1)}
        legality = {str(value): sum(row['units'] for row in packets if row['live_prefix_legal'] == value) for value in (-2, -1, 0, 1)}
        key_fields = ('mode', *flags, 'indexed', 'member', 'live_prefix_legal', 'boarding_station', 'intent_service', 'intent_boarding_order')
        left = collections.Counter()
        for row in packets:
            if row['loadable_left']:
                key = tuple(row[field] for field in key_fields) + (bool(row['new_unprocessed_units']),)
                left[key] += row['loadable_left']
        services[str(service)] = dict(departures=sum(row['service'] == service for row in checks), cargo_departure_observations=len(stops),
            partial_departures=len({(row['tick'], row['vehicle']) for row in stops if row['partial']}),
            boarded_target=sum(row['boarded_target'] for row in stops), waiting_passenger_opportunities=sum(row['waiting_units'] for row in stops),
            waiting_packet_opportunities=sum(row['waiting_packets'] for row in stops), states=states, membership=membership, live_prefix_legality=legality,
            reserved_target_passenger_opportunities=sum(row['units'] for row in virtual), reserved_target_packet_opportunities=len(virtual),
            combined_waiting_passenger_opportunities=sum(row['waiting_units'] for row in stops) + sum(row['units'] for row in virtual),
            reserved_states={mode: sum(row['units'] for row in virtual if row['mode'] == index) for index, mode in enumerate(modes)} |
                {flag: sum(row['units'] for row in virtual if row[flag]) for flag in ('early', 'expired')} |
                {'new_unprocessed': sum(row['new_unprocessed_units'] for row in virtual)},
            target_reservations_cancelled=sum(row['target_reserved'] for row in after),
            native_eligible_target_reservations_cancelled=sum(row['eligible_reserved'] for row in after),
            potential_loadable_after_cancel=sum(row['potential_loadable_after_cancel'] for row in after),
            free_before=distribution([row['free_before'] for row in stops]), free_at_departure=distribution([row['free'] for row in stops]),
            physical_free_before=distribution([row['physical_free_before'] for row in stops]), physical_free_at_departure=distribution([row['physical_free'] for row in stops]),
            reserved_before=distribution([row['reserved_before'] for row in stops]), reserved_at_departure=distribution([row['reserved'] for row in stops]),
            eligible_remaining=sum(row['eligible_remaining'] for row in stops), loadable_left=sum(row['loadable_left'] for row in stops),
            oldest_event_ticks=max((row['oldest_event_ticks'] for row in stops), default=0),
            loadable_left_states=[dict(zip((*key_fields, 'has_new_unprocessed'), key), passenger_opportunities=count) for key, count in left.most_common()])
        services[str(service)]['by_order'] = {}
        for order in sorted({row['order'] for row in stops}):
            visits = [row for row in stops if row['order'] == order]
            candidates = [row for row in packets if row['order'] == order]
            services[str(service)]['by_order'][str(order)] = dict(
                departures=len({(row['tick'], row['vehicle']) for row in visits}),
                boarded_target=sum(row['boarded_target'] for row in visits),
                waiting_passenger_opportunities=sum(row['waiting_units'] for row in visits),
                member_passenger_opportunities=sum(row['units'] for row in candidates if row['member'] == 1),
                native_prefix_legal_passenger_opportunities=sum(row['units'] for row in candidates if row['live_prefix_legal'] == 1),
                eligible_remaining=sum(row['eligible_remaining'] for row in visits),
                loadable_left=sum(row['loadable_left'] for row in visits),
                free_at_departure=distribution([row['free'] for row in visits]),
                physical_free_at_departure=distribution([row['physical_free'] for row in visits]),
                reserved_at_departure=distribution([row['reserved'] for row in visits]))
    executions = rows(directory, 'target-order-executions.csv')
    transitions = rows(directory, 'target-order-transitions.csv')
    patterns = rows(directory, 'target-patterns.csv')
    replans = rows(directory, 'target-expiry-replans.csv')
    pending = rows(directory, 'target-expiry-pending.csv')
    if any(not row[field] for row in checks for field in ('database_save_unchanged', 'rng_unchanged', 'tick_unchanged')):
        raise ValueError('A departure observer changed database Work/save, RNG or StateTicks')
    units = sum(row['units'] for row in replans)
    latency = dict(count=units, minimum=min(row['latency_ticks'] for row in replans), maximum=max(row['latency_ticks'] for row in replans),
                   mean=sum(row['units'] * row['latency_ticks'] for row in replans) / units) if units else None
    return dict(input_sha256=process['input_sha256'], binary_sha256=process['binary_sha256'],
        ticks=process['actual-ticks.json']['end_tick'] - process['actual-ticks.json']['start_tick'], services=services,
        service_528=dict(order_dispatch_counts=dict(sorted(collections.Counter(row['order'] for row in executions).items())),
            observed_order_transitions=dict(sorted(collections.Counter(row['order'] for row in transitions if not row['initial_observation']).items())),
            initial_observations=[row for row in transitions if row['initial_observation']],
            order_33_pattern=[row for row in patterns if row['service'] == 528 and row['order'] == 33]),
        read_only_departure_checks=len(checks), read_only_packet_queries=len(waiting), expiry_replan_latency=latency, expiry_pending_units=sum(row['units'] for row in pending),
        expiry_pending_max_age=max((row['age_ticks'] for row in pending), default=0),
        notes=['Waiting/state/leftover totals count repeated departure opportunities, not distinct people.',
               'State flags overlap; the five routing modes partition waiting units.',
               'Available waiting and virtual reservations are reported separately; their combined total includes every local loading consist at station 3393.',
               'other_intent includes valid bound reservations as well as older intent kinds; raw kind/current-intent fields distinguish them.',
               'potential_loadable_after_cancel is a counterfactual bound using native legality immediately before leaving and space after reservation cancellation; it is not a later boarding opportunity.',
               'live_prefix_legal tests a resolved ride with normalized readiness/age; can_board uses the actual packet.',
               'live_prefix_legal=-1 means no concrete candidate ride; -2 means membership succeeded but no ride resolved. Neither is a native-prefix rejection.',
               'free subtracts RemainingCount (kept cargo plus reservations); physical_free subtracts all physically stored cargo.',
               'oldest_event_ticks=-1 means no existing event due time is available; the raw due tick is retained.',
               'Leftover allocation follows the existing primary/sequence order; unindexed rows use packet ID as an explicit diagnostic proxy.',
               'Order dispatches are actual UpdateOrderDest calls, including conditional attempts; PBS forecasts are excluded.',
               'Expiry latencies cover observed expiry/replan pairs; pending units are right-censored at the measurement end.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_text(json.dumps(summarize(args.directory), indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
