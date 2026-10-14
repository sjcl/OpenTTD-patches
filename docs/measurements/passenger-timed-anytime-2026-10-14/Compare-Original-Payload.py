"""Verify that the original observation changes only recorded build revisions.

For the archived OTTX saves, normalize exactly two occurrences of one changed,
equally sized current-build revision. Keep every historical revision and every
other payload byte. Saves, decompressed data and assets remain outside Git.
"""
import argparse
import hashlib
import json
import lzma
import pathlib
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("baseline", type=pathlib.Path)
p.add_argument("original", type=pathlib.Path)
p.add_argument("--output", type=pathlib.Path, required=True)
a = p.parse_args()


def read(path):
    raw = path.read_bytes()
    if raw[:4] != b"OTTX":
        raise ValueError("Expected an archived OTTX save")
    return raw[:8], bytearray(lzma.decompress(raw[8:])), hashlib.sha256(raw).hexdigest()


old_header, old, old_hash = read(a.baseline)
new_header, new, new_hash = read(a.original)
pattern = rb"jgrpp-ya-[A-Za-z0-9.+_-]+"
old_all, new_all = re.findall(pattern, old), re.findall(pattern, new)
old_changed = sorted(set(old_all) - set(new_all))
new_changed = sorted(set(new_all) - set(old_all))
if len(old_changed) != 1 or len(new_changed) != 1:
    raise ValueError("Expected exactly one distinct changed current-build revision")
old_revision, new_revision = old_changed[0], new_changed[0]
if len(old_revision) != len(new_revision) or old.count(old_revision) != 2 or new.count(new_revision) != 2:
    raise ValueError("Expected two equally sized occurrences of each changed revision")
for payload, revision in ((old, old_revision), (new, new_revision)):
    cursor = 0
    for _ in range(2):
        position = payload.index(revision, cursor)
        cursor = position + len(revision)
        payload[position:cursor] = b"R" * len(revision)
result = dict(old_save_sha256=old_hash, original_save_sha256=new_hash,
              old_revision=old_revision.decode(), original_revision=new_revision.decode(),
              headers_identical=old_header == new_header, decompressed_bytes=[len(old), len(new)],
              payload_identical_except_revision=old == new,
              normalized_payload_sha256=[hashlib.sha256(old).hexdigest(), hashlib.sha256(new).hexdigest()],
              normalization="Replace the two occurrences of the changed, equally sized current-build revision only; historical revisions and every other byte must match.")
a.output.write_bytes((json.dumps(result, indent=2) + "\n").encode())
print(json.dumps(result, indent=2))
if not result["headers_identical"] or not result["payload_identical_except_revision"]:
    raise ValueError("Original observation changed more than the recorded revision")
