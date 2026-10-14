"""Reconstruct the exact archived package from hash-matching local files only.

Never overwrite source assets. Refuse missing or changed assets/input/config.
The independent workspace snapshot survives cleanup of other chats' worktrees.
"""
import argparse
import hashlib
import json
import pathlib
import shutil


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def matching(paths, expected):
    return next((p for p in paths if p.is_file() and sha(p) == expected), None)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("expected", type=pathlib.Path)
    p.add_argument("output", type=pathlib.Path)
    p.add_argument("--roots", nargs="+", required=True, type=pathlib.Path)
    p.add_argument("--saves", nargs="+", required=True, type=pathlib.Path)
    p.add_argument("--configs", nargs="+", required=True, type=pathlib.Path)
    a = p.parse_args()
    expected = json.loads(a.expected.read_text(encoding="utf-8-sig"))
    selections, missing = [], []
    for entry in expected["assets"]:
        relative = pathlib.PurePosixPath(entry["path"])
        if relative.is_absolute() or ".." in relative.parts or pathlib.PureWindowsPath(entry["path"]).drive:
            raise ValueError("Asset path must stay inside the package")
        original = matching((root / entry["path"] for root in a.roots), entry["sha256"])
        if original is None:
            missing.append(entry["path"])
        else:
            selections.append((original, a.output / entry["path"]))
    original_save = matching(a.saves, expected["input_sha256"])
    original_config = matching(a.configs, expected["config_sha256"])
    if original_save is None:
        missing.append("input.sav")
    if original_config is None:
        missing.append("openttd.cfg")
    if missing:
        raise ValueError("No hash-matching source: " + ", ".join(missing))
    a.output.mkdir(parents=True, exist_ok=False)
    for original, target in selections + [(original_save, a.output / "input.sav"), (original_config, a.output / "openttd.cfg")]:
        target.parent.mkdir(parents=True, exist_ok=True)
        # Copy, not junctions/hard links to other chats' potentially removed files.
        shutil.copy2(original, target)
    for entry in expected["assets"]:
        if sha(a.output / entry["path"]) != entry["sha256"]:
            raise ValueError("Source changed during copy: " + entry["path"])
    if sha(a.output / "input.sav") != expected["input_sha256"] or sha(a.output / "openttd.cfg") != expected["config_sha256"]:
        raise ValueError("Input/config changed during copy")
    expected["recovered_exact_archived_package"] = True
    expected["asset_source"] = str(a.output.resolve())
    (a.output / "package.json").write_text(json.dumps(expected, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"assets": len(selections), "input_sha256": sha(a.output / "input.sav"),
                      "config_sha256": sha(a.output / "openttd.cfg"), "asset_manifest_sha256": expected["asset_manifest_sha256"]}, indent=2))
