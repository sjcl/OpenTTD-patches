"""Summarize physical passenger samples; keep censored trips separate.

Usage: python passenger_transport_summary.py output.json run-directory ...
The trace follows one person in selected packets. Trip distributions are sample
statistics; transport-population.csv independently counts all passenger movements.
"""
import collections
import csv
import json
import math
import pathlib
import re
import statistics
import sys


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return dict(n=len(values), mean=statistics.mean(values), median=statistics.median(values),
                p95=values[math.ceil(.95 * len(values)) - 1],
                p99=values[math.ceil(.99 * len(values)) - 1], maximum=values[-1])


def read(path):
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as stream:
        return [{key: int(value) if key not in ("services", "stop_counts", "stations") else value
                 for key, value in row.items()} for row in csv.DictReader(stream)]


def route_shape(stations):
    path = []
    for station in stations:
        if not path or path[-1] != station:
            path.append(station)
    if len(path) > 1 and path[-1] == path[0]:
        path.pop()
    if len(path) < 2:
        return set(path), set(), set()
    edges = {tuple(sorted((station, path[(i + 1) % len(path)]))) for i, station in enumerate(path)}
    turns = {station for i, station in enumerate(path) if path[i - 1] == path[(i + 1) % len(path)]}
    return set(path), edges, turns


def structural_short_turns(patterns):
    shapes = {service: route_shape(path) for service, path in patterns.items()}
    at_station = collections.defaultdict(set)
    for service, (stations, _, _) in shapes.items():
        for station in stations:
            at_station[station].add(service)
    result = {}
    for service, (stations, edges, turns) in shapes.items():
        if len(turns) != 2:
            continue
        candidates = set.intersection(*(at_station[station] for station in turns))
        matches = []
        for candidate in candidates:
            long_stations, long_edges, long_turns = shapes[candidate]
            if stations < long_stations and edges <= long_edges and turns - long_turns:
                matches.append(candidate)
        if matches:
            result[service] = sorted(matches, key=int)
    return result


def group_class(path):
    for group in path.split('/'):
        for name, pattern in (("limited_express", r"特急|limited.?express"), ("rapid", r"快速|rapid"),
                              ("local", r"普通|各停|各駅|緩行|local"), ("semi_express", r"準急"), ("express", r"急行|express")):
            if re.search(pattern, group, re.I):
                return name
    return "unclassified"


def population_summary(directory):
    path = directory / 'transport-population.csv'
    if not path.exists():
        return None
    with path.open(newline='', encoding='utf-8') as stream:
        rows = list(csv.DictReader(stream))
    fields = ['boarded', 'transferred', 'delivered', 'known_od_boarded', 'known_od_transferred', 'known_od_delivered']
    result = {key:sum(int(row[key]) for row in rows) for key in fields}
    by_class, known_by_class = collections.Counter(), collections.Counter()
    patterns, unique, route_keys = {}, {}, {}
    train_total = 0
    for index, row in enumerate(rows):
        if not int(row['train']):
            continue
        train_total += int(row['boarded'])
        category = group_class(row['group_path'])
        by_class[category] += int(row['boarded'])
        known_by_class[category] += int(row['known_od_boarded'])
        route = tuple(map(int, row['route_stations'].split(';'))) if row['route_stations'] else ()
        if route not in unique:
            key = str(len(unique))
            unique[route] = key
            patterns[key] = route
        route_keys[index] = unique[route]
    short_turns = structural_short_turns(patterns)
    result.update(train_boarded=train_total, train_boarded_by_group_class=by_class,
                  known_od_train_boarded_by_group_class=known_by_class,
                  train_classified_fraction=1 - by_class['unclassified'] / train_total if train_total else None,
                  train_boarded_on_structural_short_turns=sum(int(row['boarded']) for i, row in enumerate(rows) if route_keys.get(i) in short_turns),
                  known_od_train_boarded_on_structural_short_turns=sum(int(row['known_od_boarded']) for i, row in enumerate(rows) if route_keys.get(i) in short_turns))
    return result


def summarize(directory):
    process_path = directory / 'process.json'
    focused = process_path.exists() and json.loads(process_path.read_text(encoding='utf-8')).get('focused_od_trace', False)
    delivered = read(directory / "transport-delivered.csv")
    pending = read(directory / "transport-pending.csv")
    lost = read(directory / "transport-lost.csv")
    all_rows = delivered + pending + lost
    for row in all_rows:
        if row["waiting_ticks"] != row["origin_waiting_ticks"] + row["transfer_waiting_ticks"]:
            raise ValueError(f"Inconsistent wait accounting in {directory}")
    metadata = (directory / "transport-trace-meta.txt").read_text(encoding="utf-8")
    selected = int(re.search(r"^selected=(\d+)$", metadata, re.M)[1])
    meta = {key: int(value) for key, value in re.findall(r"^(\w+)=(\d+)$", metadata, re.M)}
    fresh_match = re.search(r"^fresh_selected=(\d+)$", metadata, re.M)
    fresh_started = int(fresh_match[1]) if fresh_match else None
    if len(all_rows) > selected:
        raise ValueError(f"More terminal/pending samples than sample starts in {directory}")
    if all('sample_id' in row for row in all_rows) and len({row['sample_id'] for row in all_rows}) != len(all_rows):
        raise ValueError(f"Duplicate sample fate in {directory}")
    original_arrival = lambda row: bool(row['stations']) and int(row['stations'].split(';')[-1]) == row['destination']
    complete = [row for row in delivered if row["complete_origin"] and row['origin'] != 65535 and original_arrival(row)]
    destination_mismatches = [row for row in delivered if row['complete_origin'] and not original_arrival(row)]
    censored = [row for row in pending if row["complete_origin"]]
    metrics = ("riding_ticks", "waiting_ticks", "origin_waiting_ticks", "transfer_waiting_ticks", "distance_x16", "transfers", "revisits", "returns", "observed_misses", "maximum_wait")
    od = collections.defaultdict(list)
    od_rows = collections.defaultdict(list)
    for row in complete:
        key = f'{row["origin"]}:{row["destination"]}'
        od[key].append(row["at"] - row["born"])
        od_rows[key].append(row)
    services, stop_counts = collections.Counter(), collections.Counter()
    for row in delivered + pending:
        services.update(row["services"].split(";") if row["services"] else [])
        stop_counts.update(row["stop_counts"].split(";") if row["stop_counts"] else [])
    classes = collections.defaultdict(set)
    routes = collections.defaultdict(set)
    patterns = {}
    short_turns = set()
    manifest = directory / "transport-services.csv"
    if manifest.exists():
        with manifest.open(newline="", encoding="utf-8") as stream:
            for vehicle in csv.DictReader(stream):
                route = list(map(int, vehicle.get("route_stations", "").split(";"))) if vehicle.get("route_stations") else []
                routes[vehicle["service"]].update(route)
                patterns[vehicle["service"]] = route
                if re.search(r"区間|折返|折り返し|short.turn", vehicle["group_path"], re.I):
                    short_turns.add(vehicle["service"])
                category = group_class(vehicle["group_path"])
                classes[vehicle["service"]].add(category)
    by_class = collections.Counter()
    for service, count in services.items():
        labels = classes.get(service, {"unclassified"})
        by_class[next(iter(labels)) if len(labels) == 1 else "mixed_groups"] += count
    structural = structural_short_turns(patterns)
    departure_arrivals = [row for row in delivered if row.get('origin_departure', -1) >= row['born'] and original_arrival(row)]
    initial_arrivals = [row for row in delivered if row.get('initial') and original_arrival(row)]
    initial = {}
    for fate, rows in [('delivered', delivered), ('pending', pending), ('lost', lost)]:
        for row in rows:
            if row.get('initial'):
                key = f"{row['initial_packet']}:{row['initial_offset']}"
                if key in initial:
                    raise ValueError(f"Duplicate initial person in {directory}: {key}")
                initial[key] = dict(row, fate=('delivered_elsewhere' if fate == 'delivered' and not original_arrival(row) else fate))
    waiting_now = [row for row in pending if not row.get('onboard', 0)]
    waiting_od = collections.defaultdict(list)
    for row in waiting_now:
        od_key = f"{row['origin']}:{row['destination']}"
        waiting_od[od_key].append(row['maximum_wait'])
    population = population_summary(directory)
    population_balance = None
    if population and 'generated_passengers' in meta:
        population_balance = dict(initial=meta['initial_passengers'], generated=meta['generated_passengers'],
                                  delivered=population['delivered'], remaining=meta['remaining_passengers'],
                                  other_removals=meta['initial_passengers'] + meta['generated_passengers'] - population['delivered'] - meta['remaining_passengers'],
                                  initial_known=meta['initial_known_passengers'], assigned_known=meta['assigned_units'],
                                  delivered_known=population['known_od_delivered'], remaining_known=meta['remaining_known_passengers'],
                                  known_removals_or_destination_invalidation=meta['initial_known_passengers'] + meta['assigned_units'] - population['known_od_delivered'] - meta['remaining_known_passengers'])
    return dict(run=directory.name,
                sampling_method=('target_od_packet_representatives' if focused else 'hash_one_in_64_packet_representatives'),
                delivered_samples=len(delivered), complete_trip_samples=len(complete),
                population_transport=population, population_balance=population_balance,
                original_destination_mismatch_samples=len(destination_mismatches),
                original_destination_mismatch_examples=destination_mismatches[:20],
                selected_sample_starts=selected,
                fresh_sample_starts=fresh_started,
                unaccounted_fresh_sample_starts=(fresh_started - sum(r['complete_origin'] for r in all_rows)) if fresh_started is not None else None,
                unaccounted_sample_starts=selected - len(all_rows),
                lost_samples=len(lost), lost_new_samples=sum(r['complete_origin'] for r in lost),
                all_samples_observed_misses=sum(r['observed_misses'] for r in all_rows),
                fresh_samples_observed_misses=sum(r['observed_misses'] for r in all_rows if r['complete_origin']),
                initial_sample_starts=meta.get('initial_selected'), initial_fates=collections.Counter(r['fate'] for r in initial.values()),
                _initial=initial,
                observed_origin_departure_to_arrival_ticks=distribution([r['at'] - r['origin_departure'] for r in departure_arrivals]),
                initial_remaining_to_arrival_ticks=distribution([r['at'] - r['born'] for r in initial_arrivals]),
                pending_all_metrics={key: distribution([r[key] for r in pending]) for key in metrics},
                pending_waiting_now_samples=len(waiting_now),
                pending_waiting_now_maximum_wait_ticks=distribution([r['maximum_wait'] for r in waiting_now]),
                pending_longest_wait_od=sorted([dict(od=key, **distribution(values)) for key, values in waiting_od.items()], key=lambda row:(-row['maximum'], row['od']))[:30],
                pending_samples=len(pending), right_censored_new_samples=len(censored),
                trip_ticks=distribution([r["at"] - r["born"] for r in complete]),
                metrics={key: distribution([r[key] for r in complete]) for key in metrics},
                right_censored_metrics={key: distribution([r[key] for r in censored]) for key in metrics},
                right_censored_elapsed_ticks=distribution([r["at"] - r["born"] for r in censored]),
                pending_wait_ticks=distribution([r["maximum_wait"] for r in censored]),
                completed_od_ticks={key: distribution(value) for key, value in sorted(od.items())},
                completed_od_means={key:{metric:statistics.mean(row[metric] for row in group)
                                        for metric in ['distance_x16', 'transfers', 'riding_ticks', 'transfer_waiting_ticks']}
                                    for key, group in sorted(od_rows.items())},
                sampled_boardings_by_service=services, sampled_boardings_by_order_count=stop_counts,
                sampled_boardings_by_group_class=by_class,
                elapsed_minus_ride_wait_ticks=distribution([r["at"] - r["born"] - r["riding_ticks"] - r["waiting_ticks"] for r in complete]),
                trace_classified_boarding_fraction=(1 - (by_class["unclassified"] + by_class["mixed_groups"]) / sum(by_class.values())) if by_class else None,
                sampled_boardings_on_named_short_turn_groups=sum(services[s] for s in short_turns),
                sampled_boardings_on_structural_short_turns=sum(services[s] for s in structural),
                structural_short_turn_services=structural,
                complete_single_board_trip_outside_service_route=sum(r["boardings"] == 1 and r["destination"] not in routes.get(r["services"], {r["destination"]}) for r in complete),
                new_samples_with_revisit=sum(r['complete_origin'] and r["revisits"] > 0 for r in all_rows),
                new_samples_with_immediate_return=sum(r['complete_origin'] and r["returns"] > 0 for r in all_rows),
                lost_new_samples_with_revisit=sum(r['complete_origin'] and r['revisits'] > 0 for r in lost),
                lost_new_samples_with_immediate_return=sum(r['complete_origin'] and r['returns'] > 0 for r in lost),
                trace_metadata=metadata)


def compare_initial(left, right):
    a, b = left['_initial'], right['_initial']
    if a.keys() != b.keys():
        raise ValueError('Initial sampled people differ between fixed-input runs')
    fates, times, ratios, departure_ratios = collections.Counter(), [], [], []
    physical_delta = collections.defaultdict(list)
    examples = []
    for key in a:
        x, y = a[key], b[key]
        fates[f"{x['fate']}->{y['fate']}"] += 1
        if x['fate'] == y['fate'] == 'delivered':
            for metric in ['distance_x16', 'transfers', 'riding_ticks', 'origin_waiting_ticks', 'transfer_waiting_ticks', 'revisits', 'returns']:
                physical_delta[metric].append(y[metric] - x[metric])
            old, new = x['at'] - x['born'], y['at'] - y['born']
            times.append(new - old)
            if old:
                ratios.append(new / old)
                examples.append(dict(person=key, od=f"{x['origin']}:{x['destination']}", left_ticks=old, right_ticks=new, ratio=new / old))
            if x.get('origin_departure', -1) >= x['born'] and y.get('origin_departure', -1) >= y['born'] and x['at'] > x['origin_departure']:
                departure_ratios.append((y['at'] - y['origin_departure']) / (x['at'] - x['origin_departure']))
    return dict(n=len(a), fates=fates, both_delivered_elapsed_delta_ticks=distribution(times),
                both_delivered_physical_delta={key:distribution(values) for key,values in physical_delta.items()},
                both_delivered_elapsed_ratio=distribution(ratios),
                both_observed_origin_departure_to_arrival_ratio=distribution(departure_ratios),
                slowest_ratios=sorted(examples, key=lambda row:-row['ratio'])[:30])


def main():
    output, *runs = map(pathlib.Path, sys.argv[1:])
    result = dict(runs=[summarize(run) for run in runs],
                  limitations="Each run reports its sampling_method: ordinary runs use deterministic hash sampling of approximately one eligible packet in 64; independent focused runs select all packet representatives for the three target ODs. Each follows one person at the packet midpoint, capped at 65536 starts; neither is a population-weighted travel-time estimate. Do not pool focused and ordinary samples. Initial known-OD people are enrolled before settings change and matched by saved packet ID and offset. Loaded trips are left-censored; fresh full-trip and observed-origin-departure metrics are separate. Pending trips are right-censored; erasure/reduction without delivery is lost. All passenger movement totals use actual vehicle cargo, excluding reservations; population removals are the inventory residual, not a diagnosis of their cause. Service classes use actual vehicles' group names at movement; sampled service summaries use the end manifest. Observed misses cover only queried indexed candidates with positive free capacity, which other passengers may consume, and may undercount collective cache hits. Structural short turns conservatively require two reversal stops, a proper subset of a longer nominal route's stops and edges, and at least one internal reversal. Order count is not a service class. Physical distance is Manhattan displacement in 1/16-tile units.")
    if len(result["runs"]) == 2:
        left, right = result["runs"]
        if left['sampling_method'] != right['sampling_method']:
            raise ValueError('Focused and ordinary packet samples require separate reports')
        common = []
        for od in left["completed_od_ticks"].keys() & right["completed_od_ticks"].keys():
            a, b = left["completed_od_ticks"][od], right["completed_od_ticks"][od]
            if min(a["n"], b["n"]) >= 3 and a["mean"]:
                x, y = left['completed_od_means'][od], right['completed_od_means'][od]
                common.append(dict(od=od, left=a, right=b, mean_ratio=b["mean"] / a["mean"],
                                   left_physical_means=x, right_physical_means=y,
                                   distance_ratio=y['distance_x16'] / x['distance_x16'] if x['distance_x16'] else None,
                                   transfers_delta=y['transfers'] - x['transfers']))
        result["matched_od"] = sorted(common, key=lambda r: (-r["mean_ratio"], r["od"]))
        result["matched_od_mean_ratio"] = distribution([r["mean_ratio"] for r in common])
        result["matched_od_at_least_ten_mean_ratio"] = distribution([r["mean_ratio"] for r in common if min(r["left"]["n"], r["right"]["n"]) >= 10])
        result['matched_od_distance_ratio'] = distribution([r['distance_ratio'] for r in common if r['distance_ratio'] is not None])
        result['matched_od_transfers_delta'] = distribution([r['transfers_delta'] for r in common])
        result['paired_initial_people'] = compare_initial(left, right)
    for run in result['runs']:
        run.pop('_initial')
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
