"""短暂暂停指定 WindowMark 线程读取栈候选地址，finally 恢复；不控制桌面。"""
import ctypes as c, struct, sys, json
from ctypes import wintypes as w
pid,tid=map(int,sys.argv[1:3]);k=c.WinDLL('kernel32',use_last_error=True);d=c.WinDLL('dbghelp',use_last_error=True)
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE
k.OpenThread.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenThread.restype=w.HANDLE
for name in ['CloseHandle','SuspendThread','ResumeThread']:
    getattr(k,name).argtypes=[w.HANDLE]
k.GetThreadContext.argtypes=[w.HANDLE,c.c_void_p]
k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
d.SymInitialize.argtypes=[w.HANDLE,c.c_char_p,w.BOOL];d.SymInitialize.restype=w.BOOL
d.SymFromAddr.argtypes=[w.HANDLE,c.c_uint64,c.POINTER(c.c_uint64),c.c_void_p]
d.SymCleanup.argtypes=[w.HANDLE]
process=k.OpenProcess(0x410,False,pid);thread=k.OpenThread(0x1a,False,tid)
assert process and thread, c.get_last_error()
raw=c.create_string_buffer(1250);address=(c.addressof(raw)+15)&~15
c.c_uint32.from_address(address+48).value=0x10000b
stack=c.create_string_buffer(16384);count=c.c_size_t()
assert k.SuspendThread(thread)!=0xffffffff,c.get_last_error()
try:
    assert k.GetThreadContext(thread,address),c.get_last_error()
    rsp=c.c_uint64.from_address(address+152).value;rip=c.c_uint64.from_address(address+248).value
    ok=k.ReadProcessMemory(process,rsp,stack,len(stack),c.byref(count))
finally:k.ResumeThread(thread)
k.CloseHandle(thread)
assert d.SymInitialize(process,sys.argv[3].encode(),True),c.get_last_error()
def symbol(addr):
    info=c.create_string_buffer(88+1024)
    struct.pack_into('<I',info,0,88);struct.pack_into('<I',info,80,1023)
    disp=c.c_uint64()
    if not d.SymFromAddr(process,addr,c.byref(disp),info):return None
    name=info.raw[84:].split(b'\0',1)[0].decode(errors='replace')
    return dict(address=hex(addr),name=name,offset=hex(disp.value))
rows=[dict(kind='instruction',value=symbol(rip),rip=hex(rip),rsp=hex(rsp))]
for i in range(0,count.value-8,8):
    value=struct.unpack_from('<Q',stack,i)[0]
    if value<0x10000:continue
    resolved=symbol(value)
    if resolved: rows.append(dict(stack_offset=hex(i),value=resolved))
d.SymCleanup(process);k.CloseHandle(process)
print(json.dumps(rows,indent=2))
