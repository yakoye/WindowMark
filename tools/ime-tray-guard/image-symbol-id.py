"""只读 PE 映像标识与指定 RVA 字节。"""
from pathlib import Path
import struct,uuid,json,sys,hashlib
p=Path(sys.argv[1]);d=p.read_bytes();pe=struct.unpack_from('<I',d,60)[0]
sections,opt_size=struct.unpack_from('<H',d,pe+6)[0],struct.unpack_from('<H',d,pe+20)[0]
opt=pe+24;size=struct.unpack_from('<I',d,opt+56)[0];table=opt+opt_size
def offset(rva):
    for i in range(sections):
        pos=table+i*40;vsize,va,rawsize,raw=struct.unpack_from('<IIII',d,pos+8)
        if va<=rva<va+max(vsize,rawsize):return raw+rva-va
    raise ValueError('RVA outside sections')
rva,n=struct.unpack_from('<II',d,opt+112+6*8);ids=[]
for i in range(n//28):
    row=struct.unpack_from('<IIHHIIII',d,offset(rva)+i*28)
    if row[4]==2 and d[row[7]:row[7]+4]==b'RSDS':
        v=d[row[7]:row[7]+row[5]];guid=uuid.UUID(bytes_le=v[4:20]);age=struct.unpack_from('<I',v,20)[0]
        ids.append(dict(guid=str(guid),age=age,key=guid.hex.upper()+format(age,'X'),name=v[24:].split(b'\0',1)[0].decode(),signature=v[:24].hex()))
bytes_at={hex(int(a,16)):d[offset(int(a,16)):offset(int(a,16))+32].hex() for a in sys.argv[2:]}
print(json.dumps(dict(file=str(p),sha256=hashlib.sha256(d).hexdigest(),image_size=hex(size),symbols=ids,bytes=bytes_at),indent=2))
