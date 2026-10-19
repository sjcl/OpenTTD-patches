from pathlib import Path
import urllib.request, json, hashlib, zipfile, io
BASE=Path(__file__).resolve().parent/'toolchain-new'
BASE.mkdir(exist_ok=True)
if not (BASE/'catalog.json').exists():
    channel=json.loads(urllib.request.urlopen('https://aka.ms/vs/stable/channel',timeout=30).read())
    (BASE/'channel.json').write_text(json.dumps(channel,indent=2))
    manifest=next(p for p in channel['channelItems'] if p['id']=='Microsoft.VisualStudio.Manifests.VisualStudio')['payloads'][0]
    (BASE/'catalog.json').write_bytes(urllib.request.urlopen(manifest['url'],timeout=60).read())
catalog=json.loads((BASE/'catalog.json').read_bytes())
default=next(p for p in catalog['packages'] if p['id']=='Microsoft.VisualCpp.Tools.HostX64.TargetX64')
compiler_id=next(iter(default['dependencies']))
ids=(compiler_id,compiler_id.replace('.TargetX64.base','.TargetX64.Res.base'))
metadata=[]
for name in ids:
    p=next(p for p in catalog['packages'] if p['id']==name and p.get('language') in (None,'en-US'))
    for payload in p['payloads']:
        data=urllib.request.urlopen(payload['url'],timeout=60).read()
        actual=hashlib.sha256(data).hexdigest()
        assert actual==payload['sha256'], (payload['fileName'],actual,len(data))
        (BASE/payload['fileName']).write_bytes(data)
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            files=[]
            for item in archive.infolist():
                if not item.filename.startswith('Contents/') or item.is_dir(): continue
                target=(BASE/'extracted'/item.filename[len('Contents/'):]).resolve()
                assert target.is_relative_to((BASE/'extracted').resolve())
                target.parent.mkdir(parents=True,exist_ok=True)
                target.write_bytes(archive.read(item))
                files.append(str(target.relative_to(BASE)))
        metadata.append(dict(package=p['id'],version=p['version'],payload=payload,actual_sha256=actual,actual_size=len(data),files=files))
(BASE/'acquisition.json').write_text(json.dumps(metadata,indent=2))
print('Verified and extracted',len(metadata),'official MSVC packages')
