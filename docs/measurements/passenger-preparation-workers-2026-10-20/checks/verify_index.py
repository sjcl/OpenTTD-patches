"""Verify the staged evidence and compiled source inputs against their SHA-256s."""
import hashlib
import json
from pathlib import Path
import subprocess

DATA = Path(__file__).resolve().parents[1]
ROOT = DATA.parents[2]
PREFIX = DATA.relative_to(ROOT).as_posix() + "/"


def main():
    manifest = json.loads((DATA / "manifest.json").read_text())
    expected = {PREFIX + name: item["sha256"] for name, item in manifest.items()}
    identity = json.loads((DATA / "build-identity.json").read_text())
    for name, digest in identity["source_files"].items():
        assert hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == digest, name
    expected.update(identity["source_git_lf_sha256"])
    names = sorted(expected)
    result = subprocess.check_output(["git", "cat-file", "--batch"], cwd=ROOT,
                                     input=("\n".join(":" + name for name in names) + "\n").encode())
    offset = 0
    for name in names:
        end = result.index(b"\n", offset)
        header = result[offset:end].split()
        assert len(header) == 3 and header[1] == b"blob", (name, header)
        size = int(header[2])
        payload = result[end + 1:end + 1 + size]
        assert hashlib.sha256(payload).hexdigest() == expected[name], name
        offset = end + 1 + size + 1
    tracked = set(subprocess.check_output(["git", "diff", "--cached", "--name-only", "--", PREFIX], cwd=ROOT, text=True).splitlines())
    assert tracked == {PREFIX + name for name in manifest} | {PREFIX + "manifest.json"}, sorted(tracked)
    tree = subprocess.check_output(["git", "write-tree", "--prefix=src/"], cwd=ROOT, text=True).strip()
    assert tree == identity["src_tree"]
    for binary in identity["binaries"]:
        with Path(binary["backup"]).open("rb") as stream:
            assert hashlib.file_digest(stream, "sha256").hexdigest() == binary["sha256"]
    print(f"Verified {len(manifest)} staged evidence blobs, {len(identity['source_files'])} compiled source inputs, src tree {tree}, and independent binary backups.")


if __name__ == "__main__":
    main()
