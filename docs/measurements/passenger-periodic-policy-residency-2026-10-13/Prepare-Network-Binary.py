"""Copy a Windows GUI executable with console stdio for the loopback harness.
Only the PE subsystem field changes; all executable code is identical.
"""
import argparse
import hashlib
import pathlib
import struct
p = argparse.ArgumentParser()
p.add_argument("source", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
a = p.parse_args()
assert a.source.resolve() != a.output.resolve()
data = bytearray(a.source.read_bytes())
pe = struct.unpack_from("<I", data, 0x3c)[0]
assert data[pe:pe + 4] == b"PE\0\0"
assert struct.unpack_from("<H", data, pe + 24)[0] in (0x10b, 0x20b)
offset = pe + 24 + 68
assert struct.unpack_from("<H", data, offset)[0] == 2
struct.pack_into("<H", data, offset, 3)
a.output.write_bytes(data)
print({"source_sha256": hashlib.sha256(a.source.read_bytes()).hexdigest(),
       "console_sha256": hashlib.sha256(data).hexdigest(), "field_offset": offset})
