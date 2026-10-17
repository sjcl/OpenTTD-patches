"""Export the additional range-kernel evidence without rewriting older cohorts."""
import hashlib,json,pathlib,shutil,subprocess
root=pathlib.Path(__file__).resolve().parents[1]
source=root/'build-msvc/split-range-validation'
dest=root/'docs/measurements/passenger-precomputed-range-spans-2026-10-16'
assert json.loads((source/'completed.json').read_text())['success']
assert json.loads((source/'restored.json').read_text())['restored']
runtime=root/'build-msvc/span-runtime-validation'
assert json.loads((runtime/'completed.json').read_text())['success']
assert json.loads((runtime/'restored.json').read_text())['restored']
dest.mkdir(exist_ok=True)
def copy(p,target):
 target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
for p in source.rglob('*'):
 if p.is_file() and (p.suffix in ('.json','.log','.cpp','.h','.py','.cmake') or p.name == 'span-assembly.txt'):
  copy(p,dest/p.relative_to(source))
for group, target in [('split-range-lambda-rejected','lambda-rejected'),('split-range-array-rejected','array-rejected'),('cold-span-probe','prefix-probe')]:
 extra=root/'build-msvc'/group
 for p in extra.rglob('*'):
  if p.is_file() and (p.suffix in ('.json','.log','.cpp','.h','.py','.cmake') or p.name == 'span-assembly.txt'):
   copy(p,dest/target/p.relative_to(extra))
for p in (root/'build-msvc/split-range-counted/table-build').glob('PassengerTable*.*'):
 copy(p,dest/'timing-sources'/p.name)
copy(root/'build-msvc/split-range-counted/table-build/benchmark.cmake',dest/'timing-sources/benchmark.cmake')
for name in ['passenger_table_build_prepare.py','passenger_table_build_benchmark.cpp','passenger_table_build_heaps.h','passenger_table_build_simd.h','passenger_table_build_run.py']:
 copy(root/'utils'/name,dest/'method'/name)
for name in ['run-split-range-validation.py','collect-split-range-validation.py','run-cold-span-probe.py','summarize-split-ranges.py','verify-span-artifacts.py']:
 copy(root/'build-msvc'/name,dest/'method'/name)
copy(root/'build-msvc/run-span-runtime-validation.py',dest/'method/run-span-runtime-validation.py')
for p in runtime.glob('*'):
 if p.is_file() and p.suffix in ('.json','.log'):
  copy(p,dest/'runtime-validation'/p.name)
for name in ['process.json','actual-ticks.json','save-integrity.json','on-1-console.txt']:
 copy(runtime/'retained-actual'/name,dest/'runtime-validation/retained-actual'/name)
for name in ['game_start.scr','passenger_benchmark_end.scr']:
 copy(runtime/'retained-actual/scripts'/name,dest/'runtime-validation/retained-actual/scripts'/name)
copy(runtime/'readback/result.json',dest/'runtime-validation/readback/result.json')
for name in ['save-integrity.json','console.txt']:
 copy(runtime/'readback/retained-actual'/name,dest/'runtime-validation/readback/retained-actual'/name)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest=dict(before='5dc3b990ffadf8e134ba3318b1b03647356a4125',adopted_checkpoint='d7af532165489277745dafa23f7555ac9640be0d',selected_kernel='split scalar',source_head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
 sources={name:sha(root/name) for name in ['src/passenger_precomputed.cpp','src/passenger_precomputed.h']},
 fixture=json.loads((root/'build-msvc/real-fixture/source.json').read_text()),
 product_binary_sha256=sha(root/'build-msvc/Release/openttd.exe'),
 note='Range span splitting only. Same Work/cursors and graph identity; no save version or publication rule changes. SIMD remains isolated in measurement TUs unless explicitly adopted.')
(dest/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
(dest/'artifact-sha256.json').write_text(json.dumps({p.relative_to(dest).as_posix():sha(p) for p in sorted(dest.rglob('*'))
 if p.is_file() and p.name!='artifact-sha256.json'},indent=2)+'\n')
print(dest)
