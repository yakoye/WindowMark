"""只读进程窗口与等待链，不移动鼠标，不抓屏。"""
import ctypes as c, json, sys
from ctypes import wintypes as w
pid=int(sys.argv[1]); u=c.WinDLL('user32',use_last_error=True); a=c.WinDLL('advapi32',use_last_error=True)
u.GetWindowThreadProcessId.argtypes=[w.HWND,c.POINTER(w.DWORD)]
u.IsWindowVisible.argtypes=[w.HWND];u.IsWindowEnabled.argtypes=[w.HWND]
u.GetWindowTextW.argtypes=[w.HWND,w.LPWSTR,c.c_int];u.GetClassNameW.argtypes=[w.HWND,w.LPWSTR,c.c_int]
u.SendMessageTimeoutW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM,w.UINT,w.UINT,c.POINTER(c.c_size_t)]
callback=c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
windows=[];threads=set()
@callback
def visit(hwnd,_):
    owner=w.DWORD();tid=u.GetWindowThreadProcessId(hwnd,c.byref(owner))
    if owner.value==pid:
        title=c.create_unicode_buffer(1024);cls=c.create_unicode_buffer(256)
        u.GetWindowTextW(hwnd,title,1024);u.GetClassNameW(hwnd,cls,256)
        response=c.c_size_t();ok=u.SendMessageTimeoutW(hwnd,0,0,0,2,150,c.byref(response))
        windows.append(dict(hwnd=int(hwnd),tid=tid,title=title.value,cls=cls.value,visible=bool(u.IsWindowVisible(hwnd)),enabled=bool(u.IsWindowEnabled(hwnd)),responsive=bool(ok)))
        threads.add(tid)
    return True
u.EnumWindows.argtypes=[callback,w.LPARAM];u.EnumWindows(visit,0)
class Lock(c.Structure):
    _fields_=[('name',w.WCHAR*128),('timeout',c.c_int64),('alertable',w.BOOL)]
class Thread(c.Structure):
    _fields_=[('pid',w.DWORD),('tid',w.DWORD),('wait',w.DWORD),('switches',w.DWORD)]
class Value(c.Union):_fields_=[('lock',Lock),('thread',Thread)]
class Node(c.Structure):_fields_=[('type',c.c_int),('status',c.c_int),('value',Value)]
a.OpenThreadWaitChainSession.argtypes=[w.DWORD,c.c_void_p];a.OpenThreadWaitChainSession.restype=w.HANDLE
a.GetThreadWaitChain.argtypes=[w.HANDLE,c.c_size_t,w.DWORD,w.DWORD,c.POINTER(w.DWORD),c.POINTER(Node),c.POINTER(w.BOOL)]
a.CloseThreadWaitChainSession.argtypes=[w.HANDLE]
session=a.OpenThreadWaitChainSession(0,None);chains=[]
threads.update(int(t) for t in sys.argv[2:])
for tid in threads:
    count=w.DWORD(16);nodes=(Node*16)();cycle=w.BOOL()
    ok=a.GetThreadWaitChain(session,0,0,tid,c.byref(count),nodes,c.byref(cycle))
    chains.append(dict(tid=tid,ok=bool(ok),error=c.get_last_error() if not ok else 0,cycle=bool(cycle.value),nodes=[dict(type=n.type,status=n.status,pid=n.value.thread.pid if n.type==8 else None,tid=n.value.thread.tid if n.type==8 else None,name=n.value.lock.name if n.type!=8 else None) for n in nodes[:count.value]] if ok else []))
a.CloseThreadWaitChainSession(session)
print(json.dumps(dict(pid=pid,windows=windows,chains=chains),ensure_ascii=False,indent=2))
