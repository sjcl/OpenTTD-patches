"""Verify the exported raw evidence and its staged Git blobs."""
import hashlib,json,pathlib,subprocess
root=pathlib.Path(__file__).resolve().parents[1]
folder=root/'docs/measurements/passenger-precomputed-build-2026-10-16'
manifest=json.loads((folder/'artifact-sha256.json').read_text())
all_files={p.relative_to(folder).as_posix() for p in folder.rglob('*') if p.is_file() and p.name!='artifact-sha256.json'}
assert all_files==set(manifest),(all_files-set(manifest),set(manifest)-all_files)
records=subprocess.check_output(['git','ls-files','--stage','-z','--',str(folder)],cwd=root)
staged={}
for record in records.split(b'\0'):
 if not record:continue
 meta,path=record.split(b'\t',1);mode,blob,stage=meta.split()
 assert stage==b'0',record
 staged[path.decode('utf-8')]=blob.decode()
json_count=0
for name in list(manifest)+['artifact-sha256.json']:
 p=folder/name;data=p.read_bytes()
 if name!='artifact-sha256.json':assert hashlib.sha256(data).hexdigest()==manifest[name],name
 if p.suffix=='.json':json.loads(data);json_count+=1
 blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
 assert staged.get(p.relative_to(root).as_posix())==blob,('staged bytes differ',name)
print(f'{len(manifest)} artifact hashes, {json_count} JSON files, {len(manifest)+1} staged blobs verified.')
