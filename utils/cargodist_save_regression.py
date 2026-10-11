#!/usr/bin/env python3
"""Headless CargoDist save compatibility and corrupt-table regression checks.

Usage: python utils/cargodist_save_regression.py EXE OLD_SAVE RELEASE_SAVE OUTPUT_DIR
The supplied EXE must have a baseset and languages installed beside it.
All generated saves/configuration/logs stay in OUTPUT_DIR. No input is modified.
"""

import copy
import lzma
from pathlib import Path
import subprocess
import sys


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, count):
        result = self.data[self.pos:self.pos + count]
        assert len(result) == count
        self.pos += count
        return result

    def gamma(self):
        first = self.take(1)[0]
        count = 0
        while first & (0x80 >> count):
            count += 1
        assert count <= 4
        value = first & (0x7f >> count)
        return (value << (count * 8)) | int.from_bytes(self.take(count), 'big')


def gamma(value):
    count = max(0, (value.bit_length() - 1) // 7)
    assert count <= 4
    prefix = (0xff << (8 - count)) & 0xff
    return bytes([prefix | (value >> (count * 8))]) + (value & ((1 << (count * 8)) - 1)).to_bytes(count, 'big')


def schema(reader):
    fields = []
    while True:
        kind = reader.take(1)[0]
        if not kind:
            break
        name = reader.take(reader.gamma()).decode()
        fields.append([name, kind, None])
    for field in fields:
        if field[1] & 15 == 11:
            field[2] = schema(reader)
    return fields


WIDTH = {1: 1, 2: 1, 3: 2, 4: 2, 5: 4, 6: 4, 7: 8, 8: 8, 9: 2}


def decode(reader, fields):
    result = {}
    for name, kind, children in fields:
        base = kind & 15
        count = reader.gamma() if kind & 16 else 1
        if base == 11:
            value = [decode(reader, children) for _ in range(count)]
        elif base == 10:
            value = reader.take(count)
        else:
            value = [int.from_bytes(reader.take(WIDTH[base]), 'big', signed=base in (1, 3, 5, 7)) for _ in range(count)]
            if not kind & 16:
                value = value[0]
        result[name] = value
    return result


def encode(obj, fields):
    result = bytearray()
    for name, kind, children in fields:
        base, value = kind & 15, obj[name]
        if kind & 16:
            result += gamma(len(value))
        if base == 11:
            for item in value:
                result += encode(item, children)
        elif base == 10:
            result += value
        else:
            for item in value if kind & 16 else [value]:
                result += item.to_bytes(WIDTH[base], 'big', signed=base in (1, 3, 5, 7))
    return bytes(result)


def read_save(path):
    data = path.read_bytes()
    assert data[:4] in (b'OTTX', b'OTTN')
    raw = lzma.decompress(data[8:]) if data[:4] == b'OTTX' else data[8:]
    return data[4:8], raw


def table(raw, name):
    # Restrict this fixture utility to the named current table format. The
    # source fixtures are loaded by the game first, before any table decoding.
    start = raw.index(name.encode() + b'\x03')
    reader = Reader(raw[start + 5:])
    entries = []
    while True:
        count = reader.gamma()
        if count == 0:
            break
        entries.append(reader.take(count - 1))
    header_reader = Reader(entries[0])
    fields = schema(header_reader)
    assert header_reader.pos == len(entries[0])
    rows = []
    for payload in entries[1:]:
        if not payload:
            rows.append(None)
            continue
        row_reader = Reader(payload)
        rows.append(decode(row_reader, fields))
        assert row_reader.pos == len(payload)
    return start, start + 5 + reader.pos, entries[0], fields, rows


def encode_schema(fields):
    result = bytearray()
    for name, kind, children in fields:
        encoded = name.encode()
        result += bytes([kind]) + gamma(len(encoded)) + encoded
    result += b'\x00'
    for name, kind, children in fields:
        if children is not None:
            result += encode_schema(children)
    return bytes(result)


def replace_table(raw, name, rows, drop=frozenset()):
    start, end, header, fields, _ = table(raw, name)
    if drop:
        def filtered(items):
            return [[label, kind, filtered(children) if children is not None else None]
                    for label, kind, children in items if label not in drop]
        fields = filtered(fields)
        header = encode_schema(fields)
    result = name.encode() + b'\x03' + gamma(len(header) + 1) + header
    for row in rows:
        payload = encode(row, fields) if row is not None else b''
        result += gamma(len(payload) + 1) + payload
    return raw[:start] + result + b'\x00' + raw[end:]


def remove_feature(raw, name):
    # SLXI RIFF: version, flags, count; each feature has flags/version/name,
    # optional extra data and optional chunk ID list. Keep unrelated entries.
    start = raw.index(b'SLXI')
    length = int.from_bytes(raw[start + 4:start + 8], 'big')
    reader = Reader(raw[start + 8:start + 8 + length])
    header = reader.take(8)
    count = int.from_bytes(reader.take(4), 'big')
    entries = []
    for _ in range(count):
        begin = reader.pos
        flags = int.from_bytes(reader.take(4), 'big')
        reader.take(2)
        label = reader.take(reader.gamma()).decode()
        if flags & 4:
            reader.take(int.from_bytes(reader.take(4), 'big'))
        if flags & 8:
            reader.take(4 * int.from_bytes(reader.take(4), 'big'))
        if label != name:
            entries.append(reader.data[begin:reader.pos])
    assert reader.pos == length and len(entries) == count - 1
    payload = header + len(entries).to_bytes(4, 'big') + b''.join(entries)
    return raw[:start] + b'SLXI' + len(payload).to_bytes(4, 'big') + payload + raw[start + 8 + length:]


def main():
    exe, old, released, directory = [Path(value).resolve() for value in sys.argv[1:]]
    directory.mkdir(parents=True, exist_ok=True)
    scripts = directory / 'scripts'
    scripts.mkdir(exist_ok=True)
    config = directory / 'config.cfg'
    config.write_text('[misc]\nsavegame_format = none\nsmall_font = Arial\nmedium_font = Arial\nlarge_font = Arial\nmono_font = Consolas\n[gui]\nautosave = off\nautosave_on_exit = false\n')

    def run(source, label, expect=None):
        output = directory / (label + '-output.sav')
        output.unlink(missing_ok=True)
        script = '\n'.join([
            'echo CARGODIST_SAVE_TEST',
            'set linkgraph.passenger_transfer_timetables true',
            'set linkgraph.transfer_penalty_same_mode 2',
            'set linkgraph.transfer_penalty_mode_change 5',
            f'save "{output.with_suffix("").as_posix()}"',
            'quit', ''])
        (scripts / 'game_start.scr').write_text(script)
        # On Windows, -d opens a console and replaces the inherited stderr pipe.
        # Load errors are level zero and are emitted without enabling debug.
        command = [str(exe), '-c', str(config), '-x', '-v', 'null', '-s', 'null', '-m', 'null', '-g', str(source)]
        process = subprocess.run(command, capture_output=True, timeout=60)
        log = (process.stdout + process.stderr).decode(errors='replace')
        (directory / (label + '.log')).write_text(log, encoding='utf-8')
        if expect:
            assert expect in log, (label, process.returncode, log[-2500:])
            print('PASS rejected', label, expect)
        else:
            assert process.returncode == 0 and output.exists(), (label, log[-2500:])
            print('PASS roundtrip', label)
        return output

    run(old, 'legacy-openttd')
    converted = run(released, 'published-0.73.3+2')
    reloaded = run(converted, 'new-format')
    version, raw = read_save(converted)
    assert table(read_save(reloaded)[1], 'LGRS')[4] == table(raw, 'LGRS')[4], 'Generation counter changed on reload'
    legacy = remove_feature(raw, 'linkgraph_route_penalty')
    for name in ('ORDL', 'BKOR', 'LGRP', 'LGRJ', 'LGRS'):
        legacy = replace_table(legacy, name, table(legacy, name)[4],
                               {'edge_routes', 'cargodist_cost_scale', 'cargodist_cost_add', 'cargodist_route_generation'})
    path = directory / 'legacy-jgrpp-no-route-feature.sav'
    path.write_bytes(b'OTTN' + version + legacy)
    result = run(path, 'legacy-jgrpp-no-route-feature')
    for order in table(read_save(result)[1], 'ORDL')[4]:
        if order:
            assert (order['cargodist_cost_scale'], order['cargodist_cost_add']) == (100, 0)
    _, _, _, _, rows = table(raw, 'LGRP')
    graph_index = next(index for index, row in enumerate(rows) if row and any(any(edge['to'] != source for edge in node['edges']) for source, node in enumerate(row['nodes'])))
    graph = rows[graph_index]
    from_node = next(index for index, node in enumerate(graph['nodes']) if any(edge['to'] != index for edge in node['edges']))
    edge = next(edge for edge in graph['nodes'][from_node]['edges'] if edge['to'] != from_node)
    assert edge['to'] != from_node
    _, _, _, _, orders = table(raw, 'ORDL')
    route_id = next(index for index, order in enumerate(orders) if order)
    order = orders[route_id]
    order['cargodist_cost_scale'] = 175
    order['cargodist_cost_add'] = 23
    date = max(edge['last_unrestricted_update'], edge['last_restricted_update'], 0)
    record = {'from': from_node, 'to': edge['to'], 'route': route_id, 'vehicle_type': 0,
              'cost_scale': 175, 'cost_add': 23, 'capacity': 10, 'last_update': date,
              'generation': order['cargodist_route_generation']}
    graph['edge_routes'] = [record]
    route_save = directory / 'valid-routes.sav'
    valid = replace_table(replace_table(raw, 'ORDL', orders), 'LGRP', rows)
    route_save.write_bytes(b'OTTN' + version + valid)
    preserved = run(route_save, 'valid-route-roundtrip')
    _, saved_raw = read_save(preserved)
    saved_routes = table(saved_raw, 'LGRP')[4][graph_index]['edge_routes']
    assert saved_routes == [record], saved_routes
    saved_order = table(saved_raw, 'ORDL')[4][route_id]
    assert (saved_order['cargodist_cost_scale'], saved_order['cargodist_cost_add']) == (175, 23)
    variant = copy.deepcopy(rows)
    variant[graph_index]['edge_routes'][0]['generation'] = 0
    path = directory / 'legacy-unbound-route.sav'
    path.write_bytes(b'OTTN' + version + replace_table(valid, 'LGRP', variant))
    result = run(path, 'legacy-unbound-route')
    assert not table(read_save(result)[1], 'LGRP')[4][graph_index]['edge_routes']
    for field, value, expected in [
        ('vehicle_type', 255, 'Invalid link graph route metadata'),
        ('route', 65535, 'Invalid link graph route metadata'),
        ('cost_scale', 0, 'Invalid link graph route metadata'),
        ('cost_scale', 1001, 'Invalid link graph route metadata'),
        ('cost_add', 366, 'Invalid link graph route metadata'),
        ('capacity', 0, 'Invalid link graph route metadata'),
        ('last_update', -2, 'Invalid link graph route metadata'),
        ('from', 65535, 'Invalid link graph route nodes'),
        ('to', 65535, 'Invalid link graph route nodes'),
        ('to', from_node, 'Invalid link graph route nodes'),
    ]:
        variant = copy.deepcopy(rows)
        variant[graph_index]['edge_routes'][0][field] = value
        label = f'invalid-{field}-{value}'
        path = directory / (label + '.sav')
        path.write_bytes(b'OTTN' + version + replace_table(valid, 'LGRP', variant))
        run(path, label, expected)
    variant = copy.deepcopy(rows)
    variant[graph_index]['edge_routes'].append(copy.deepcopy(record))
    path = directory / 'duplicate-route.sav'
    path.write_bytes(b'OTTN' + version + replace_table(valid, 'LGRP', variant))
    run(path, 'duplicate-route', 'Duplicate link graph route')
    missing = next((source, to) for source in range(len(graph['nodes'])) for to in range(len(graph['nodes']))
                   if source != to and all(edge['to'] != to for edge in graph['nodes'][source]['edges']))
    variant = copy.deepcopy(rows)
    variant[graph_index]['edge_routes'][0].update(zip(('from', 'to'), missing))
    path = directory / 'missing-route-edge.sav'
    path.write_bytes(b'OTTN' + version + replace_table(valid, 'LGRP', variant))
    run(path, 'missing-route-edge', 'Link graph route references a missing edge')
    variant = copy.deepcopy(rows)
    variant[graph_index]['nodes'][from_node]['last_update'] = -2
    path = directory / 'invalid-node-date.sav'
    path.write_bytes(b'OTTN' + version + replace_table(valid, 'LGRP', variant))
    run(path, 'invalid-node-date', 'Invalid link graph node update date')
    variant = copy.deepcopy(rows)
    variant[graph_index]['edge_routes'][0]['last_update'] = date + 100000
    path = directory / 'future-route-date.sav'
    path.write_bytes(b'OTTN' + version + replace_table(valid, 'LGRP', variant))
    result = run(path, 'future-route-date')
    assert table(read_save(result)[1], 'LGRP')[4][graph_index]['edge_routes'][0]['last_update'] < date + 100000

    class OversizedList(list):
        def __len__(self):
            return 4 * 1024 * 1024 + 1

    variant = copy.deepcopy(rows)
    variant[graph_index]['edge_routes'] = OversizedList()
    path = directory / 'oversized-routes.sav'
    path.write_bytes(b'OTTN' + version + replace_table(valid, 'LGRP', variant))
    run(path, 'oversized-routes', 'List exceeds storage size')
    for field, value, expected in [
        ('to', 65535, 'Invalid link graph edge destination'),
        ('capacity', 0, 'Invalid link graph capacity or travel time'),
        ('usage', edge['capacity'] + 1, 'Invalid link graph capacity or travel time'),
        ('travel_time_sum', edge['capacity'] * (1 << 32), 'Invalid link graph capacity or travel time'),
        ('last_unrestricted_update', -2, 'Invalid link graph update date'),
    ]:
        variant = copy.deepcopy(rows)
        target = next(item for item in variant[graph_index]['nodes'][from_node]['edges'] if item['to'] != from_node)
        target[field] = value
        label = f'invalid-edge-{field}'
        path = directory / (label + '.sav')
        path.write_bytes(b'OTTN' + version + replace_table(valid, 'LGRP', variant))
        run(path, label, expected)
    for field, value in [('cargodist_cost_scale', 0), ('cargodist_cost_add', 366)]:
        variant = copy.deepcopy(orders)
        variant[route_id][field] = value
        label = 'invalid-order-' + field
        path = directory / (label + '.sav')
        path.write_bytes(b'OTTN' + version + replace_table(valid, 'ORDL', variant))
        run(path, label, 'Invalid order list CargoDist cost')
    for value in (0, 1 << 63):
        variant = copy.deepcopy(orders)
        variant[route_id]['cargodist_route_generation'] = value
        label = f'invalid-order-generation-{value}'
        path = directory / (label + '.sav')
        path.write_bytes(b'OTTN' + version + replace_table(valid, 'ORDL', variant))
        run(path, label, 'Invalid CargoDist route generation')
    schedule = table(raw, 'LGRS')[4]
    schedule[0]['cargodist_route_generation'] = 123456789
    path = directory / 'saved-generation-counter.sav'
    path.write_bytes(b'OTTN' + version + replace_table(valid, 'LGRS', schedule))
    result = run(path, 'saved-generation-counter')
    assert table(read_save(result)[1], 'LGRS')[4][0]['cargodist_route_generation'] == 123456789
    schedule[0]['cargodist_route_generation'] = 1 << 63
    path = directory / 'invalid-generation-counter.sav'
    path.write_bytes(b'OTTN' + version + replace_table(valid, 'LGRS', schedule))
    run(path, 'invalid-generation-counter', 'Invalid CargoDist generation counter')

    # BKOR is normally transmitted by a network server and removed after an
    # ordinary game load. Exercise the real table reader with one minimal record.
    def empty_record(fields):
        return {name: ([] if kind & 15 == 11 else b'' if kind & 15 == 10 else [] if kind & 16 else 0)
                for name, kind, children in fields}
    backup = empty_record(table(raw, 'BKOR')[3])
    backup.update(tile=0, group=65535, cargodist_cost_scale=175, cargodist_cost_add=23)
    path = directory / 'valid-backup.sav'
    path.write_bytes(b'OTTN' + version + replace_table(valid, 'BKOR', [backup]))
    run(path, 'valid-backup-table')
    for field, value in [('cargodist_cost_scale', 0), ('cargodist_cost_add', 366)]:
        variant = copy.deepcopy(backup)
        variant[field] = value
        label = 'invalid-backup-' + field
        path = directory / (label + '.sav')
        path.write_bytes(b'OTTN' + version + replace_table(valid, 'BKOR', [variant]))
        run(path, label, 'Invalid backup CargoDist cost')
    print('All CargoDist save regression checks passed.')


if __name__ == '__main__':
    main()
