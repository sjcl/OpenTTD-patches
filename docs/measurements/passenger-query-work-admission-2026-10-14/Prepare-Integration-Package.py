"""Freeze the same map/GRFs/config with language packs for the current GUI strings."""
import argparse
import importlib.util
import json
import pathlib
import shutil

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('runner', HERE.parent / 'passenger-routing-profile-2026-10-13/Run-Profile.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=pathlib.Path)
p.add_argument('frozen', type=pathlib.Path)
p.add_argument('--directory', default='integration-latest-package')
a = p.parse_args()
build, frozen = a.build.resolve(), a.frozen.resolve()
out = build / a.directory
out.mkdir(exist_ok=False)
for name in runner.ASSET_DIRS:
    source = build / 'lang' if name == 'lang' else frozen / name
    if source.is_dir():
        shutil.copytree(source, out / name, copy_function=shutil.copy2 if name == 'lang' else runner.hardlink_or_copy)
for name in ['input.sav', 'openttd.cfg']:
    shutil.copy2(frozen / name, out / name)
# The multiplayer helper reuses build/package. Replace owned language files by
# unlinking first: an old hard link must never overwrite the frozen source file.
for target in [build / 'package/lang', build / 'Release/lang']:
    target.mkdir(parents=True, exist_ok=True)
    assert target.resolve().is_relative_to(build)
    for source in (out / 'lang').iterdir():
        if not source.is_file():
            continue
        destination = target / source.name
        if destination.exists():
            destination.unlink()
        shutil.copy2(source, destination)
metadata = {'input_sha256': runner.sha256(out / 'input.sav'), 'config_sha256': runner.sha256(out / 'openttd.cfg'),
            'frozen_source': str(frozen), 'language_source': str(build / 'lang'),
            'language_sha256': {path.name: runner.sha256(path) for path in (out / 'lang').iterdir() if path.is_file()}}
(out / 'package.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
print(json.dumps(metadata, indent=2))
