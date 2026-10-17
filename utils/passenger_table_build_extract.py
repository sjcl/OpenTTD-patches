#!/usr/bin/env python3
"""Extract JPSD and station positions from an OTTX table-format benchmark save.

Read-only, fixture-specific extraction. Reject unfamiliar layouts rather than
guessing. Does not replace the game's save loader or rewrite save files.
"""
import argparse
import hashlib
import json
import lzma
import pathlib


def gamma(data, pos):
    first = data[pos]
    extra = 0
    while first & (128 >> extra):
        extra += 1
    assert extra <= 4
    value = first & (127 >> extra)
    for byte in data[pos + 1:pos + extra + 1]:
        value = (value << 8) | byte
    return value, pos + extra + 1


def table(data, name):
    pos = data.index(name)
    assert data.count(name) == 1 and data[pos + 4] == 3
    length, pos = gamma(data, pos + 5)
    header = data[pos:pos + length - 1]
    return header, pos + length - 1


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("save", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    raw = args.save.read_bytes()
    assert raw[:4] == b"OTTX"
    data = lzma.decompress(raw[8:])
    pos = data.rindex(b"JPSD")
    riff = int.from_bytes(data[pos + 4:pos + 8], "big")
    assert riff & 0x0F000000 == 0
    length = (riff & 0xFFFFFF) | ((riff >> 28) << 24)
    blob = data[pos + 8:pos + 8 + length]
    assert len(blob) == length and len(blob) % 8 == 0 and int.from_bytes(blob[:8], "little") in (8, 9, 10, 11)
    header, pos = table(data, b"MAPS")
    assert header == b"\x06\x05dim_x\x06\x05dim_y\x00"
    length, pos = gamma(data, pos)
    assert length == 9
    width, height = int.from_bytes(data[pos:pos + 4], "big"), int.from_bytes(data[pos + 4:pos + 8], "big")
    header, pos = table(data, b"STNN")
    assert header.startswith(b"\x02\x0afacilities\x1b\x06normal\x1b\x08waypoint")
    assert b"\x06\x02xy" in header
    stations = []
    index = 0
    while True:
        length, pos = gamma(data, pos)
        if length == 0:
            break
        end = pos + length - 1
        if length > 1:
            facilities = data[pos]
            normal, at = gamma(data, pos + 1)
            assert normal in (0, 1)
            if normal == 0:
                waypoint, at = gamma(data, at)
                assert waypoint == 1
            base, at = gamma(data, at)
            assert base == 1
            tile = int.from_bytes(data[at:at + 4], "big")
            assert tile < width * height
            if normal:
                stations.append((index, tile % width, tile // width))
        index += 1
        pos = end
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "database.bin").write_bytes(blob)
    (args.output / "stations.txt").write_text(f"{width} {height}\n" + ''.join(f"{i} {x} {y}\n" for i, x, y in stations), encoding="utf-8")
    manifest = dict(source=str(args.save.resolve()), sha256=hashlib.sha256(raw).hexdigest(), jpsd_sha256=hashlib.sha256(blob).hexdigest(),
                    width=width, height=height, stations=len(stations), jpsd_version=blob[0])
    (args.output / "source.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest))


if __name__ == "__main__":
    main()
