"""只读集成版控制协议，核验提示语开关；不发送窗口消息、不写共享内存。"""
import argparse
import ctypes as c
import json
import struct
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--map',required=True)
p.add_argument('--out',required=True)
a=p.parse_args()
k=c.WinDLL('kernel32',use_last_error=True)
k.OpenFileMappingW.argtypes=[c.c_uint32,c.c_int,c.c_wchar_p];k.OpenFileMappingW.restype=c.c_void_p
k.MapViewOfFile.argtypes=[c.c_void_p,c.c_uint32,c.c_uint32,c.c_uint32,c.c_size_t];k.MapViewOfFile.restype=c.c_void_p
k.UnmapViewOfFile.argtypes=[c.c_void_p];k.CloseHandle.argtypes=[c.c_void_p]
h=k.OpenFileMappingW(4,False,a.map)
if not h:raise c.WinError(c.get_last_error())
view=None
try:
    view=k.MapViewOfFile(h,4,0,0,72)
    if not view:raise c.WinError(c.get_last_error())
    raw=c.string_at(view,72)
    magic,version,owner,stop=struct.unpack_from('<IIII',raw)
    if magic!=0x574d4947 or version!=3:raise RuntimeError('协议不匹配')
    state,writes,restores,failures,target=struct.unpack_from('<iiiii',raw,24)
    width,beat,result,hints=struct.unpack_from('<qqii',raw,48)
    record=dict(owner_pid=owner,target_pid=target,stop=stop,state=state,writes=writes,restores=restores,
                restore_failures=failures,width=width/1000,show_hints=bool(hints),target_heartbeat=beat,result=result)
    text=json.dumps(record,ensure_ascii=False,indent=2)
    with Path(a.out).open('x',encoding='utf-8') as out:out.write(text)
    print(text)
finally:
    if view:k.UnmapViewOfFile(view)
    k.CloseHandle(h)
