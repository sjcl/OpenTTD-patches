from pathlib import Path
import os,sys,json,re,subprocess,hashlib
sys.dont_write_bytecode=True
BASE=Path(__file__).resolve().parent
sys.path.insert(0,str(BASE/'python'))
import xxhash
ROOT=BASE.parent
WORK=ROOT/'build-windows-default/generate/pgo-training'
PROFILE=ROOT/'cmake/pgo/windows.profdata'
TOOL=Path(os.environ['ProgramFiles'])/'LLVM/bin/llvm-profdata.exe'
def path_hash(text):return format(xxhash.xxh3_64_intdigest(text.encode()) & 0xffffffff,'X')
commands=json.loads((ROOT/'build-windows-default/generate/compile_commands.json').read_text())
mapping={};origins={}
for entry in commands:
    if '-fprofile-generate' not in entry['command']:continue
    path=Path(entry['file'])
    relative=path.relative_to(ROOT).as_posix()
    if relative.endswith('/pgo-driver/null_v.cpp'):relative='src/video/null_v.cpp'
    canonical='\\openttd\\'+relative.replace('/','\\')
    old=path_hash(str(path));new=path_hash(canonical)
    assert old not in mapping or mapping[old]==new
    mapping[old]=new;origins[old]=dict(source=relative,old=old,canonical=new)
text_path=BASE/'canonical-profile.txt'
subprocess.run([str(TOOL),'merge','--text',str(PROFILE),'-o',str(text_path)],check=True)
text=text_path.read_text(encoding='utf-8')
ids={m.upper() for m in re.findall(r'\?A0x([0-9a-fA-F]+)',text)}
assert ids <= mapping.keys(), sorted(ids-mapping.keys())
new_text=re.sub(r'\?A0x([0-9a-fA-F]+)',lambda m:'?A0x'+mapping[m[1].upper()],text)
text_path.write_text(new_text,encoding='utf-8')
subprocess.run([str(TOOL),'merge',str(text_path),'-o',str(PROFILE)],check=True)
meta=PROFILE.parent/'windows.json';data=json.loads(meta.read_text())
data['profile_sha256']=hashlib.sha256(PROFILE.read_bytes()).hexdigest()
data['source_prefix_map']='/openttd (Windows separators in the Microsoft ABI hash)'
data['namespace_hash_migration']=dict(algorithm='LLVM MicrosoftMangle: low 32 bits of xxh3_64',xxhash_python_version=xxhash.VERSION,
                                    mappings=[origins[i] for i in sorted(ids)])
data['training_exit_code']=0
data['training_binary_sha256']=hashlib.sha256((WORK/'openttd.exe').read_bytes()).hexdigest()
data['note']='Shared IR profile from the recorded game simulation sources and allocator fix. Training hooks, pre-fix Kernel32 lookup/wrappers and startup initializer records are excluded. Source-root-dependent names were canonicalized for both function records and indirect-call targets. No savegame or assets are distributed.'
meta.write_text(json.dumps(data,indent=2)+'\n')
print(json.dumps(dict(profile_sha256=data['profile_sha256'],mappings=data['namespace_hash_migration']['mappings']),indent=2))
