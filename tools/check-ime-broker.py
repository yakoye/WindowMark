"""同一交互会话中只读检查任务栏及已有占位端点响应。"""
import ctypes as c,json
from ctypes import wintypes as w
u=c.WinDLL('user32',use_last_error=True)
u.FindWindowW.argtypes=[w.LPCWSTR,w.LPCWSTR];u.FindWindowW.restype=w.HWND
u.FindWindowExW.argtypes=[w.HWND,w.HWND,w.LPCWSTR,w.LPCWSTR];u.FindWindowExW.restype=w.HWND
u.GetWindowThreadProcessId.argtypes=[w.HWND,c.POINTER(w.DWORD)]
u.SendMessageTimeoutW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM,w.UINT,w.UINT,c.POINTER(c.c_size_t)]
def read(hwnd):
    pid=w.DWORD();tid=u.GetWindowThreadProcessId(hwnd,c.byref(pid));result=c.c_size_t()
    ok=u.SendMessageTimeoutW(hwnd,0,0,0,2,500,c.byref(result))
    return dict(hwnd=int(hwnd),pid=pid.value,tid=tid,responsive=bool(ok))
taskbar=u.FindWindowW('Shell_TrayWnd',None)
items=[];last=None
while True:
    last=u.FindWindowExW(w.HWND(-3),last,'WindowMark.XamlContainerTrial.LocalRoots07',None)
    if not last:break
    items.append(read(last))
print(json.dumps(dict(taskbar=read(taskbar) if taskbar else None,brokers=items),indent=2))
