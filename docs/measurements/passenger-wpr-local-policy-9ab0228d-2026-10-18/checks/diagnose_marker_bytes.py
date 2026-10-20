"""Read-only check for raw annotation strings in rejected ETLs; not a replacement for marker audit."""
from pathlib import Path
import json
import mmap

WORK=Path(__file__).resolve().parent
assert (WORK/'hyperpath/wpr-finished.txt').is_file()
result={}
for folder in ['rejected-missing-begin-marker','rejected-missing-begin-marker-2','rejected-missing-begin-marker-3']:
    p=WORK/folder/'cpu.etl';found={}
    with p.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        for encoding in ['utf-16-le','utf-8']:
            prefix='hyperpath'.encode(encoding);offset=0;matches=[]
            while (offset:=data.find(prefix,offset))>=0:
                text=data[offset:offset+96].decode(encoding,errors='replace').split('\0')[0]
                for name in ['hyperpath-begin','hyperpath-ready','hyperpath-end']:
                    if text.startswith(name):matches.append(dict(label=name,byte_offset=offset))
                offset+=len(prefix)
            found[encoding]=matches
    result[folder]=dict(bytes=p.stat().st_size,raw_string_matches=found,
        note='String presence alone does not establish a valid event or timestamp; the exact-window audit remains mandatory')
(WORK/'marker-byte-diagnostic.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2),flush=True)
