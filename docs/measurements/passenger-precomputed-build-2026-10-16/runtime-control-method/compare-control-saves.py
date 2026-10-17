"""Read two completed control saves; compare every byte outside the JPSD chunk."""
import hashlib, json, lzma, pathlib
root=pathlib.Path(__file__).resolve().parent
result={}
for mode in ['legacy-a','integrated-a']:
    path=root/('runtime-same-image-'+mode)/'result.sav'
    saved=path.read_bytes();assert saved[:4]==b'OTTX'
    raw=lzma.decompress(saved[8:]);at=raw.rindex(b'JPSD')
    descriptor=int.from_bytes(raw[at+4:at+8],'big');assert descriptor&0x0f000000==0
    size=(descriptor&0xffffff)|((descriptor>>28)<<24)
    end=at+8+size;assert end<=len(raw) and size%8==0
    assert int.from_bytes(raw[at+8:at+16],'little')==11
    view=memoryview(raw)
    result[mode]=dict(save_sha256=hashlib.sha256(saved).hexdigest(),header=saved[4:8].hex(),
        prefix_bytes=at,prefix_sha256=hashlib.sha256(view[:at]).hexdigest(),
        jpsd_bytes=size,jpsd_sha256=hashlib.sha256(view[at+8:end]).hexdigest(),
        suffix_bytes=len(raw)-end,suffix_sha256=hashlib.sha256(view[end:]).hexdigest())
    del view,raw
keys=['header','prefix_bytes','prefix_sha256','suffix_bytes','suffix_sha256']
same=all(result['legacy-a'][k]==result['integrated-a'][k] for k in keys)
output=dict(runs=result,all_bytes_outside_jpsd_identical=same,
    note='JPSD holds different engine identity and construction cursors. Other bytes include every other saved game chunk. No save file was changed.')
(root/'same-image-control/save-byte-comparison.json').write_text(json.dumps(output,indent=2)+'\n')
print(json.dumps(output,indent=2));assert same,output
