"""Complete a frozen Profile package with this build's assets and extracted OpenGFX."""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import shutil
import tarfile
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=pathlib.Path)
a = p.parse_args()
build = a.build.resolve()
source = pathlib.Path(__file__).resolve().parents[3]
package = build / "package"
for name in ("baseset", "lang"):
    shutil.copytree(build / name, package / name, dirs_exist_ok=True)
with tarfile.open(package / "content_download/baseset/4f474658-OpenGFX-0.6.1.tar") as tar:
    tar.extractall(package / "baseset", filter="data")
spec = importlib.util.spec_from_file_location("runner", source / "docs/measurements/passenger-routing-profile-2026-10-13/Run-Profile.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
metadata = json.loads((package / "package.json").read_text(encoding="utf-8"))
metadata["assets"] = runner.inventory(package)
metadata["asset_manifest_sha256"] = hashlib.sha256(json.dumps(metadata["assets"], sort_keys=True).encode()).hexdigest()
metadata["current_source_assets"] = True
runner.dump(package / "package.json", metadata)
for name in ("baseset", "lang"):
    shutil.copytree(package / name, build / "Release" / name, dirs_exist_ok=True)
print(len(metadata["assets"]), metadata["asset_manifest_sha256"])
