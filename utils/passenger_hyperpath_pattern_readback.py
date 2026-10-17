#!/usr/bin/env python3
"""Read selected published/frozen patterns from JPSD v9-v15, without game ticks.

This diagnostic accepts an extracted database.bin, not a savegame. Values and
the complete record layout are checked before output; unknown versions fail.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def read(data):
    if len(data) % 8:
        raise ValueError("JPSD records contain complete little-endian uint64 values")
    words = struct.unpack(f'<{len(data) // 8}Q', data)
    at = 0

    def take():
        nonlocal at
        if at == len(words):
            raise ValueError("Truncated JPSD")
        value = words[at]
        at += 1
        return value

    def count(maximum):
        value = take()
        if value > maximum or value > len(words) - at:
            raise ValueError("Invalid JPSD array count")
        return value

    version = take()
    if not 9 <= version <= 15:
        raise ValueError(f"Unsupported JPSD version {version}")

    def service():
        fields = ('id', 'incarnation', 'generation', 'period', 'estimated_period')
        result = {key: take() for key in fields}
        stops = []
        for _ in range(count(4096)):
            stops.append({key: take() for key in ('station', 'order', 'arrival', 'departure', 'board', 'alight', 'forced', 'continues', 'travel_quality')})
        result['stops'] = stops
        fleet = []
        for _ in range(count(65536)):
            row = {key: take() for key in ('id', 'base', 'phase_known', 'type')}
            row['capacity'] = [take() for _ in range(64)]
            fleet.append(row)
        result['vehicles'] = fleet
        result.update({key: take() for key in ('dispatch_base', 'dispatch_floor', 'dispatch_period', 'dispatch_stop')})
        result['dispatch_slots'] = [take() for _ in range(count(65536))]
        result['uncertain_dispatch_period'] = take()
        if result['period'] and result['estimated_period']:
            raise ValueError('Certified and estimated periods are mutually exclusive')
        if any(stop['continues'] > 1 or stop['travel_quality'] > 2 for stop in stops):
            raise ValueError('Invalid stop flags')
        return result

    def network():
        result = [service() for _ in range(count(65535))]
        ids = [row['id'] for row in result]
        if ids != sorted(set(ids)):
            raise ValueError('JPSD services must be in unique ID order')
        return result

    header = {key: take() for key in ('generation', 'cursor', 'sweeping', 'dirty_cursor', 'sweeping_since')}
    dirty = [(take(), take()) for _ in range(count(65535))]
    published = network()
    for _ in range(4):
        take()  # published revision, batch start, publishing, bootstrap
    for _ in range(count(65535)):
        take(); take()  # queued service ID and admission tick
    for _ in range(count(65535)):
        take()  # unsafe service ID
    building = []
    for _ in range(count(65535)):
        building.append(service()); take()  # staged admission tick
    precomputed = {key: take() for key in ('work', 'transfer', 'revision', 'refresh', 'invalidated')}
    frozen = network()
    retained = {key: take() for key in ('revision', 'transfer')}
    retained_frozen = network()
    flags = {}
    for name, introduced in [('sections', 8), ('estimates', 9), ('optimized', 10), ('integrated', 11), ('hyperpath', 12)]:
        if version >= introduced:
            flags[name] = [take(), take()]
            if any(value > 1 for value in flags[name]):
                raise ValueError('Invalid construction identity')
    if version >= 12:
        flags['policy_generation'] = [take(), take()]
    if version >= 13:
        flags['revisit_dominance'] = [take(), take()]
    if version >= 14:
        trailer = at
        flags['continuation_witness'] = [take(), take()]
        for snapshot in (published, building, frozen, retained_frozen):
            for row in snapshot:
                for offset in range(0, len(row['stops']), 21):
                    keys = take()
                    for index, stop in enumerate(row['stops'][offset:offset + 21]):
                        stop['boarding_facility'] = (keys >> (3 * index)) & 7
        expected = at - trailer
        if take() != expected:
            raise ValueError('Invalid v14 trailer length')
    if version >= 15:
        flags['flat_storage'] = [take(), take()]
        if any(value > 1 or (value and not flags['hyperpath'][index]) for index, value in enumerate(flags['flat_storage'])):
            raise ValueError('Invalid flat-storage identity')
    if at != len(words):
        raise ValueError(f'Unexpected JPSD tail: {len(words) - at} words')
    return dict(version=version, header=header, precomputed=precomputed, retained=retained, flags=flags,
                snapshots=dict(published=published, building=building, frozen=frozen, retained=retained_frozen))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('database', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--services', type=int, nargs='+', default=[524, 525, 528])
    args = parser.parse_args()
    raw = args.database.read_bytes()
    result = read(raw)
    result['database_sha256'] = hashlib.sha256(raw).hexdigest()
    result['snapshot_totals'] = {name: dict(services=len(rows), stops=sum(len(row['stops']) for row in rows))
                                 for name, rows in result['snapshots'].items()}
    result['snapshots'] = {name: [row for row in rows if row['id'] in args.services]
                           for name, rows in result['snapshots'].items()}
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
