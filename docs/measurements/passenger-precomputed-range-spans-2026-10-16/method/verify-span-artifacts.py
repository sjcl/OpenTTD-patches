"""Verify exported hashes, JSON syntax and exact staged raw blob bytes."""
import hashlib,json,pathlib,subprocess
root=pathlib.Path(__file__).resolve().parents[1]
folder=root/'docs/measurements/passenger-precomputed-range-spans-2026-10-16'
manifest=json.loads((folder/'artifact-sha256.json').read_text())
files={p.relative_to(folder).as_posix() for p in folder.rglob('*') if p.is_file() and p.name!='artifact-sha256.json'}
assert files==set(manifest)
staged={}
for record in subprocess.check_output(['git','ls-files','--stage','-z','--',str(folder)],cwd=root).split(b'\0'):
 if not record:continue
 meta,path=record.split(b'\t',1);mode,blob,stage=meta.split();assert stage==b'0'
 staged[path.decode()]=blob.decode()
json_count=0
for name in list(manifest)+['artifact-sha256.json']:
 p=folder/name;data=p.read_bytes()
 if name!='artifact-sha256.json':assert hashlib.sha256(data).hexdigest()==manifest[name],name
 if p.suffix=='.json':json.loads(data);json_count+=1
 blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
 assert staged.get(p.relative_to(root).as_posix())==blob,('staged bytes differ',name)
print(len(manifest),'artifact hashes,',json_count,'JSON files and',len(manifest)+1,'staged raw blobs verified.')
