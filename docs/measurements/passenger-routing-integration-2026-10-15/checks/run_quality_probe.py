import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
build = root / 'build-integration'
spec = importlib.util.spec_from_file_location('real', root / 'utils/passenger_precomputed_real_map.py')
real = importlib.util.module_from_spec(spec)
spec.loader.exec_module(real)
directory = build / 'quality-probe-readback'
directory.mkdir(exist_ok=False)
package = pathlib.Path('C:/Users/bebe/.codex/worktrees/6fb9/OpenTTD-patches/build-packet-events/map-package')
for name in real.PROFILE.ASSET_DIRS:
    if name != 'lang' and (package / name).is_dir():
        shutil.copytree(package / name,directory / name,copy_function=real.PROFILE.hardlink_or_copy)
shutil.copytree(build / 'lang',directory / 'lang',copy_function=real.PROFILE.hardlink_or_copy)
shutil.copy2(build / 'cold-precomputed/openttd.cfg',directory / 'openttd.cfg')
shutil.copy2(build / 'cold-precomputed/result.sav',directory / 'input.sav')
(directory / 'scripts').mkdir(exist_ok=True)
log = (directory / 'console.txt').as_posix()
(directory / 'scripts/game_start.scr').write_text(f'script {log}\npassenger_journey_stats build_precomputed\npassenger_journey_stats quality_probe\npassenger_journey_stats save_integrity\nscript\nquit\n')
binary = build / 'quality-probe/openttd.exe'
command = [str(binary),'-x','-X','-c','openttd.cfg','-g','input.sav','-v','null:until_exit','-snull','-mnull','-d','sl=2']
with (directory / 'stdout.txt').open('wb') as out, (directory / 'stderr.txt').open('wb') as err:
    subprocess.run(command,cwd=directory,env=dict(os.environ,OPENTTD_PASSENGER_MEASURE='1'),stdout=out,stderr=err,creationflags=subprocess.CREATE_NO_WINDOW,check=True,timeout=1200)
expected = json.loads((build / 'cold-precomputed/save-integrity.json').read_text())
actual = json.loads((directory / 'save-integrity.json').read_text())
mismatches = {key:[expected.get(key),actual.get(key)] for key in expected.keys() | actual.keys() if expected.get(key) != actual.get(key)}
result = dict(canonical_keys=len(expected),mismatches=mismatches,timing_used=False,
              input_sha256=hashlib.sha256((directory / 'input.sav').read_bytes()).hexdigest(),
              binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
(directory / 'result.json').write_text(json.dumps(result,indent=2)+'\n')
assert not mismatches,mismatches
print(json.dumps(result),flush=True)
