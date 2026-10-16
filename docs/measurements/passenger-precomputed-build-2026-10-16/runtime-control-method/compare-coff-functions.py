"""Compare function bytes and relocation targets in MSVC COFF objects; no timing inference."""
import argparse,hashlib,json,pathlib,struct

def functions(path):
 b=path.read_bytes();u=lambda fmt,off:struct.unpack_from('<'+fmt,b,off)
 if u('HH',0)==(0,65535):
  nsec,syms,nsyms=u('III',44);header=56;record=20;section_fmt='i';tail=18
 else:
  machine,nsec,stamp,syms,nsyms,opt,flags=u('HHIIIHH',0);assert machine==0x8664
  header=20+opt;record=18;section_fmt='h';tail=16
 assert syms and syms+nsyms*record+4<=len(b)
 strings=syms+nsyms*record
 def name(raw):
  if raw[:4]==b'\0'*4:
   at=strings+struct.unpack_from('<I',raw,4)[0];return b[at:b.index(0,at)].decode('utf-8',errors='replace')
  return raw.rstrip(b'\0').decode('utf-8',errors='replace')
 symbols={};code=[];i=0
 while i<nsyms:
  at=syms+i*record;n=name(b[at:at+8]);value=u('I',at+8)[0];section=u(section_fmt,at+12)[0];typ=u('H',at+tail-2)[0];storage,aux=u('BB',at+tail)
  symbols[i]=n
  if section>0 and typ&0x20 and storage in (2,3):code.append((n,section,value))
  i+=1+aux
 sections={}
 for i in range(nsec):
  at=header+i*40;size,raw,rel=u('III',at+16);nr=u('H',at+32)[0]
  sections[i+1]=(size,raw,rel,nr)
 result={}
 for n,sec,start in code:
  size,raw,rel,nr=sections[sec];end=min([v for _,s,v in code if s==sec and v>start]+[size]);data=bytearray(b[raw+start:raw+end]);targets=[]
  for j in range(nr):
   off,index,typ=u('IIH',rel+j*10)
   if start<=off<end:
    width={1:8,2:4,3:4,4:4,5:4,6:4,7:4,8:4,9:4,10:2,11:4,12:1,13:4,14:4,15:0,16:4}.get(typ)
    assert width is not None,(n,typ)
    data[off-start:off-start+width]=bytes(width);targets.append((off-start,typ,symbols.get(index,'aux:'+str(index))))
  result[n]=dict(size=end-start,normalized_sha256=hashlib.sha256(data).hexdigest(),relocations=targets)
 return result

if __name__=='__main__':
 a=argparse.ArgumentParser(description=__doc__);a.add_argument('old',type=pathlib.Path);a.add_argument('new',type=pathlib.Path);a.add_argument('output',type=pathlib.Path);args=a.parse_args()
 old,new=functions(args.old),functions(args.new);common=old.keys()&new.keys();same=[k for k in common if old[k]==new[k]]
 selected={k:dict(old=old[k],new=new[k],exact=old[k]==new[k],normalized_bytes_identical=old[k]['normalized_sha256']==new[k]['normalized_sha256'] and old[k]['size']==new[k]['size']) for k in sorted(common) if any(p in k for p in ['StateGameLoop','CallVehicleTicks','TrainLocoHandler','TrainController','Tick@Train'])}
 result=dict(old=str(args.old),new=str(args.new),common_functions=len(common),identical_functions=len(same),identical_function_bytes=sum(old[k]['normalized_sha256']==new[k]['normalized_sha256'] and old[k]['size']==new[k]['size'] for k in common),selected=selected,note='Relocated bytes zeroed; symbolic relocation targets compared. This describes generated code, not elapsed performance.')
 args.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='selected'}));print({k:v['exact'] for k,v in selected.items()})
