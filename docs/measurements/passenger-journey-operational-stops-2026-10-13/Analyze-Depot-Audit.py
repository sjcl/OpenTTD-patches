import collections
import json
import pathlib
import re
import hashlib
import sys

source = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).with_name('depot-audit-console.txt')
services = []
def fields(text):
    return dict(item.split('=', 1) for item in text.split('; '))
for line in source.read_text(encoding='utf-8').splitlines():
    if m := re.fullmatch(r'Depot service (\d+): (.*)', line):
        service = dict(id=int(m[1]), orders=[], **fields(m[2]))
        services.append(service)
    elif m := re.fullmatch(r'Depot audit order (\d+): (.*)', line):
        order = dict(index=int(m[1]), **fields(m[2]))
        service['orders'].append(order)
    elif line.startswith('Depot flags: '):
        order['depot'] = fields(line[len('Depot flags: '):])

def reasons(service):
    found = set()
    for order in service['orders']:
        typ = order['type']
        if typ in {'OT_IMPLICIT', 'OT_LABEL'}:
            continue
        if typ == 'OT_GOTO_DEPOT':
            if order['depot']['traversal'] != '2':
                found.add('depot_traversal_' + order['depot']['traversal'])
            if order['dispatch'] != '-1':
                found.add('depot_dispatch')
        elif typ not in {'OT_GOTO_STATION', 'OT_GOTO_WAYPOINT'}:
            found.add('unsupported_' + typ)
        if order['refit'] == 'true':
            found.add('refit_' + typ)
        if order['travel'].endswith('/false'):
            found.add('missing_travel_' + typ)
        stopping = typ == 'OT_GOTO_STATION' and not (int(order['nonstop']) & 2)
        if stopping or typ == 'OT_GOTO_DEPOT' or not order['wait'].startswith('0/'):
            if order['wait'].endswith('/false'):
                found.add('missing_wait_' + typ)
    return sorted(found)

for service in services:
    service['rejections'] = reasons(service)
    service['pre_depot_orders'] = [service['orders'][i - 1] for i, order in enumerate(service['orders']) if order['type'] == 'OT_GOTO_DEPOT']

def summarize(selected):
    depots = [order for service in selected for order in service['orders'] if order['type'] == 'OT_GOTO_DEPOT']
    preceding = [order for service in selected for order in service['pre_depot_orders']]
    return dict(
        services=len(selected),
        passenger_vehicles=sum(int(service['passenger_vehicles']) for service in selected),
        published=sum(service['published'] == 'true' for service in selected),
        live_timed=sum(service['live_period'] != '0' for service in selected),
        rejection_combinations=dict(collections.Counter(','.join(service['rejections']) for service in selected)),
        depot_orders=len(depots),
        depot_flags=dict(collections.Counter(json.dumps(order['depot'], sort_keys=True) for order in depots)),
        depot_travel_set=sum(order['travel'].endswith('/true') for order in depots),
        depot_wait_set=sum(order['wait'].endswith('/true') for order in depots),
        preceding_order_types=dict(collections.Counter(order['type'] for order in preceding)),
        preceding_station_cargo_flags=dict(collections.Counter('load=' + order['pax_load'] + ',unload=' + order['pax_unload'] for order in preceding if order['type'] == 'OT_GOTO_STATION')),
        examples=[{key: service[key] for key in ['id','vehicle','passenger_vehicles','rejections']} for service in selected[:10]],
    )

result = {
    'console_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'cargo_permission_columns': 'CargoType 0; passenger fleet uses CargoClass::Passengers',
    'all_depot_lists': summarize(services),
    'published_depot_lists': summarize([s for s in services if s['published'] == 'true']),
    'passenger_depot_lists': summarize([s for s in services if int(s['passenger_vehicles']) > 0]),
    'published_passenger_depot_lists': summarize([s for s in services if s['published'] == 'true' and int(s['passenger_vehicles']) > 0]),
    'non_depot_problem_examples': [dict(id=s['id'], vehicle=s['vehicle'], rejections=s['rejections']) for s in services if any(reason.startswith('missing_') and 'DEPOT' not in reason for reason in s['rejections'])],
}
source.with_name('depot-audit-summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps({key: result[key] for key in ['console_sha256', 'passenger_depot_lists']}, indent=2))
