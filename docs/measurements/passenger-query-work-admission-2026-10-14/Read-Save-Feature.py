"""Read the routing extension version from the real LZMA save's first SLXI chunk."""
import lzma
import pathlib
import sys


def routing_feature(path):
    with pathlib.Path(path).open('rb') as source:
        if source.read(4) != b'OTTX':
            raise ValueError('This evidence reader supports the measured LZMA saves only')
        source.read(4)
        prefix = lzma.LZMADecompressor().decompress(source.read(65536), max_length=65536)
    if prefix[:4] != b'SLXI':
        raise ValueError('Expected SLXI first')
    length = int.from_bytes(prefix[4:8], 'big')
    header = prefix[8:8 + length]
    name = b'passenger_journey_routing'
    index = header.find(name)
    if index < 7 or header[index - 1] != len(name) or header.count(name) != 1:
        raise ValueError('Unexpected SLXI record encoding')
    return int.from_bytes(header[index - 3:index - 1], 'big')


if __name__ == '__main__':
    for path in sys.argv[1:]:
        print(path, routing_feature(path))
