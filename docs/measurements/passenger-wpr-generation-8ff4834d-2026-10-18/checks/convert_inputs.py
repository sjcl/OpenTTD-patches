"""Convert the retained v15 development snapshots to the consolidated v1 wire.

Only version metadata and retired algorithm flags change; no game ticks run.
All removed flags and untouched data are inventoried for the load validation.
"""
from pathlib import Path
import hashlib
import json
import lzma
import os
import shutil
import stat
import struct
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OLD=ROOT/'build-rebaseline-88cb61e3'
sys.path.insert(0,str(ROOT/'utils'))
import passenger_precomputed_real_map as real
def sha(data):return hashlib.sha256(data).hexdigest()
def link_or_copy(source,dest):
    try:os.link(source,dest)
    except OSError:shutil.copy2(source,dest)
    return str(dest)
results={}
for mode in ['legacy','hyperpath']:
    package=OLD/'seeds'/mode
    raw=(package/'input.sav').read_bytes()
    expected=json.loads((OLD/'seed-validation.json').read_text())[mode]['input_sha256']
    assert sha(raw)==expected and raw[:4]==b'OTTX'
    original=lzma.decompress(raw[8:]);data=bytearray(original);features=[]
    for name,version,flags in [('passenger_od_assignment',3,4),('passenger_journey_routing',3,0),('passenger_periodic',17,8)]:
        label=name.encode();at=data.index(label)
        assert data[at-1]==len(label)
        assert int.from_bytes(data[at-7:at-3],'big')==flags
        assert int.from_bytes(data[at-3:at-1],'big')==version
        data[at-3:at-1]=(1).to_bytes(2,'big')
        features.append(dict(feature=name,from_version=version,to_version=1,offset=at-3))
    at=data.rindex(b'JPSD');riff=int.from_bytes(data[at+4:at+8],'big')
    assert riff&0x0F000000==0
    length=(riff&0xFFFFFF)|((riff>>28)<<24)
    wire=bytes(data[at+8:at+8+length]);assert len(wire)==length and length%8==0
    words=list(struct.unpack('<'+str(length//8)+'Q',wire));assert words[0]==15
    cursor=6;facility_count=0
    def get():
        global cursor
        value=words[cursor];cursor+=1
        return value
    def skip(n):
        global cursor
        assert n>=0 and cursor+n<=len(words)
        cursor+=n
    def service():
        global facility_count
        skip(5)
        stops=get();assert stops<=4096;skip(stops*9)
        facility_count+=(stops+20)//21
        fleet=get();assert fleet<=65536;skip(fleet*(4+64))
        skip(4);slots=get();assert slots<=65536;skip(slots)
        skip(1)
    def network(staged=False):
        count=get();assert count<65536
        for _ in range(count):
            service()
            if staged:skip(1)
        return count
    dirty=get();skip(2*dirty)
    sizes=[network()]
    skip(4);queued=get();skip(2*queued);unsafe=get();skip(unsafe)
    sizes.append(network(staged=True))
    current_at=cursor;skip(5)
    sizes.append(network())
    retained_at=cursor;skip(2)
    sizes.append(network())
    flags_at=cursor
    algorithm=words[cursor:cursor+8];skip(8)
    hyperpath=words[cursor:cursor+2];skip(2)
    policy=words[cursor:cursor+2];skip(2)
    revisit=words[cursor:cursor+2];skip(2)
    witness=words[cursor:cursor+2];skip(2)
    facilities=words[cursor:cursor+facility_count];skip(facility_count)
    assert get()==facility_count+2
    flat=words[cursor:cursor+2];skip(2)
    assert cursor==len(words)
    assert words[current_at+2]!=(1<<64)-1 and words[retained_at]==(1<<64)-1
    assert algorithm[::2]==[1,1,1,1] and revisit[0]==witness[0]==1
    assert hyperpath[0]==(mode=='hyperpath') and flat[0]==(mode=='hyperpath')
    converted=[1]+words[1:flags_at]+hyperpath+policy+facilities
    changed=struct.pack('<'+str(len(converted))+'Q',*converted)
    assert len(changed)==len(wire)-120
    new_riff=(riff&0x0F000000)|(len(changed)&0xFFFFFF)|((len(changed)>>24)<<28)
    data[at+4:at+8]=new_riff.to_bytes(4,'big')
    data[at+8:at+8+length]=changed
    dest=WORK/'inputs'/mode;dest.mkdir(parents=True,exist_ok=False)
    for folder in real.PROFILE.ASSET_DIRS:
        if (package/folder).is_dir():shutil.copytree(package/folder,dest/folder,copy_function=link_or_copy)
    shutil.copy2(package/'openttd.cfg',dest/'openttd.cfg')
    encoded=raw[:8]+lzma.compress(bytes(data),preset=1)
    (dest/'input.sav').write_bytes(encoded);(dest/'input.sav').chmod(stat.S_IREAD)
    manifest=json.loads((package/'package.json').read_text())
    manifest['source_input_sha256']=expected;manifest['input_sha256']=sha(encoded)
    (dest/'package.json').write_text(json.dumps(manifest,indent=2)+'\n')
    results[mode]=dict(source_input_sha256=expected,input_sha256=sha(encoded),state_ticks_advanced=0,
        input_uncompressed_sha256=sha(original),output_uncompressed_sha256=sha(bytes(data)),feature_versions=features,
        database=dict(from_version=15,to_version=1,input_bytes=len(wire),output_bytes=len(changed),output_sha256=sha(changed),
            networks=sizes,retired_algorithm_pairs=algorithm,retired_revisit=revisit,retired_witness=witness,retired_flat=flat,
            active_flags_match_current_hardwired_algorithms=True,retained_generation_absent=True),
        untouched_uncompressed_prefix_sha256=sha(original[:features[0]['offset']]))
    (WORK/(mode+'-converted-jpsd.bin')).write_bytes(changed)
    print(mode,results[mode],flush=True)
(WORK/'input-conversion.json').write_text(json.dumps(results,indent=2)+'\n')
