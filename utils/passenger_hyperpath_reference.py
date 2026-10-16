#!/usr/bin/env python3
"""Independent exact-rational optimal-strategies oracle for small graphs.

No game implementation, fixed-point helper or production heap is imported.
Ordinary reverse edges are (upstream, downstream, cost); boarding candidates
are (waiting node, downstream min node, risk, single-service wait, occurrence).
The event key is the full Fraction, including downstream expectations.
"""
from fractions import Fraction
import heapq
import json


def solve(sinks, edges, boards):
    incoming, boarding = {}, {}
    for upstream, downstream, cost in edges:
        incoming.setdefault(downstream, []).append((upstream, Fraction(cost)))
    for waiting, downstream, risk, wait, occurrence in boards:
        assert wait > 0
        boarding.setdefault(downstream, []).append((waiting, Fraction(risk), Fraction(1, wait), occurrence))
    labels, sums, sets, settled, trace, heap = {}, {}, {}, set(), [], []
    serial = 0

    def push(cost, kind, payload):
        nonlocal serial
        heapq.heappush(heap, (cost, kind, serial, payload))
        serial += 1

    for node in sinks:
        labels[node] = Fraction(0)
        push(Fraction(0), 1, node)
    while heap:
        key, kind, _, payload = heapq.heappop(heap)
        if kind == 0:
            waiting, frequency, occurrence = payload
            if waiting in settled:
                continue
            numerator, total_frequency = sums.get(waiting, (Fraction(1), Fraction(0)))
            before = numerator / total_frequency if total_frequency else None
            accepted = before is None or key < before
            trace.append(dict(occurrence=occurrence, x=str(key), f=str(frequency), before=str(before), accepted=accepted))
            if accepted:
                numerator += frequency * key
                total_frequency += frequency
                sums[waiting] = numerator, total_frequency
                sets.setdefault(waiting, []).append(occurrence)
                labels[waiting] = numerator / total_frequency
                assert labels[waiting] > key
                push(labels[waiting], 2, waiting)
            continue
        node = payload
        if node in settled or labels[node] != key:
            continue
        settled.add(node)
        for upstream, cost in incoming.get(node, []):
            if upstream not in labels or key + cost < labels[upstream]:
                labels[upstream] = key + cost
                push(key + cost, 1, upstream)
        for waiting, risk, frequency, occurrence in boarding.get(node, []):
            push(key + risk, 0, (waiting, frequency, occurrence))
    return labels, sets, trace


def selftest():
    results = []
    cases = [
        ("single", [(100, 100)], Fraction(200), [0]),
        ("local_express", [(100, 100), (150, 100)], Fraction(175), [0, 1]),
        ("reject_slow", [(100, 100), (210, 1)], Fraction(200), [0]),
        ("unbounded_set", [(100, 100)] * 9, Fraction(100) + Fraction(100, 9), list(range(9))),
        ("rejected_becomes_attractive", [(210, 1)], Fraction(211), [0]),
    ]
    for name, candidates, expected, accepted in cases:
        edges = [(f"ride{i}", "sink", x) for i, (x, _) in enumerate(candidates)]
        boards = [("wait", f"ride{i}", 0, w, i) for i, (_, w) in enumerate(candidates)]
        labels, sets, trace = solve(["sink"], edges, boards)
        assert labels["wait"] == expected, (name, labels)
        assert sets["wait"] == accepted, (name, sets)
        results.append(dict(name=name, cost=str(labels["wait"]), candidates=sets["wait"], trace=trace))
    # One departure node chooses the minimum of several legal alighting edges;
    # adding a second alighting choice does not create a second boarding event.
    labels, sets, trace = solve(["sink"], [("ride", "sink", 100), ("ride", "sink", 150)], [("wait", "ride", 7, 100, 42)])
    assert labels["wait"] == 207 and sets["wait"] == [42]
    results.append(dict(name="one_vehicle_multiple_alights", cost="207", candidates=[42], trace=trace))
    # A second strategy downstream, then an upstream direct/transfer choice.
    # These constants also exercise the production frozen graph test: each
    # 100-tick leg has risk ceil(220/64)=4; the direct 220-tick leg risk is 8.
    labels, sets, trace = solve(['sink'], [('bd1', 'sink', 104), ('bd2', 'sink', 104), ('ab', 'B', 104), ('ad', 'sink', 228)],
                               [('B', 'bd1', 0, 110, 1), ('B', 'bd2', 0, 110, 2), ('A', 'ab', 0, 110, 0), ('A', 'ad', 0, 230, 3)])
    assert labels['B'] == 159 and labels['A'] == Fraction(11087, 34)
    assert sets['B'] == [1, 2] and sets['A'] == [3, 0]
    results.append(dict(name='downstream_strategy', cost=str(labels['A']), downstream=str(labels['B']), candidates=sets['A'], trace=trace))
    return results


if __name__ == "__main__":
    print(json.dumps(selftest(), indent=2))
