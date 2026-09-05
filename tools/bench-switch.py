# 切换窗口时，边框多久才出现在新窗口上。
#
# 被动观察，不抢焦点：挂 EVENT_SYSTEM_FOREGROUND，记录「新窗口成为前台」到「有一个
# 边框窗口的矩形对上了它」之间的间隔。判据是矩形而不是 z 序——使用者看到的是线画在
# 哪儿，不是它排第几。
#
#   python tools\bench-switch.py 30
import ctypes
import sys
import threading
import time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND
u.GetWindow.restype = wintypes.HWND
u.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
u.GetWindowLongPtrW.restype = ctypes.c_longlong
u.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

WINEVENTPROC = ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.DWORD, wintypes.HWND,
                                  wintypes.LONG, wintypes.LONG, wintypes.DWORD,
                                  wintypes.DWORD)
P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
EVENT_SYSTEM_FOREGROUND = 0x0003
BORDER_CLASS = 'WindowMark.WindowBorder'

qpc = ctypes.c_longlong()
k.QueryPerformanceFrequency(ctypes.byref(qpc))


def now_ms():
    c = ctypes.c_longlong()
    k.QueryPerformanceCounter(ctypes.byref(c))
    return c.value * 1000.0 / qpc.value


def cls(h):
    b = ctypes.create_unicode_buffer(160)
    u.GetClassNameW(h, b, 160)
    return b.value


def pname(h):
    pid = wintypes.DWORD()
    u.GetWindowThreadProcessId(h, ctypes.byref(pid))
    hp = k.OpenProcess(0x1000, False, pid)
    if not hp:
        return '?'
    b = ctypes.create_unicode_buffer(260)
    s = wintypes.DWORD(260)
    ok = k.QueryFullProcessImageNameW(hp, 0, b, ctypes.byref(s))
    k.CloseHandle(hp)
    return b.value.rsplit('\\', 1)[-1] if ok else '?'


def rc(h):
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def dwmr(h):
    r = wintypes.RECT()
    if d.DwmGetWindowAttribute(wintypes.HWND(h), 9, ctypes.byref(r), ctypes.sizeof(r)) != 0:
        return None
    return (r.left, r.top, r.right, r.bottom)


def borders():
    out = []

    def cb(h, _):
        if u.IsWindowVisible(h) and cls(h) == BORDER_CLASS:
            out.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    return out


def covered(target):
    """有没有一个可见边框的矩形对上了这个窗口（中心距离 60 像素以内）。"""
    f = dwmr(target) or rc(target)
    cx, cy = (f[0] + f[2]) / 2, (f[1] + f[3]) / 2
    for b in borders():
        r = rc(b)
        if abs((r[0] + r[2]) / 2 - cx) + abs((r[1] + r[3]) / 2 - cy) <= 60:
            return True
    return False


lat = []
misses = [0]
lock = threading.Lock()


def watch(target, t0):
    deadline = t0 + 2000.0
    while now_ms() < deadline:
        if covered(target):
            with lock:
                lat.append(now_ms() - t0)
            return
        time.sleep(0.002)
    with lock:
        misses[0] += 1


def on_fg(hook, event, hwnd, idobj, idchild, thread, tm):
    if not hwnd or idobj != 0:
        return
    if cls(hwnd).startswith('WindowMark.'):
        return
    r = rc(hwnd)
    if (r[2] - r[0]) < 300 or (r[3] - r[1]) < 200:
        return
    threading.Thread(target=watch, args=(hwnd, now_ms()), daemon=True).start()


proc = WINEVENTPROC(on_fg)
hook = u.SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, None,
                         proc, 0, 0, 0)
if not hook:
    print('挂钩子失败')
    raise SystemExit(1)

seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 30
print('观察 %d 秒——请来回切换几个窗口。不抢焦点，不动任何窗口。' % seconds)
print()

end = now_ms() + seconds * 1000.0
msg = wintypes.MSG()
while now_ms() < end:
    while u.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
        u.TranslateMessage(ctypes.byref(msg))
        u.DispatchMessageW(ctypes.byref(msg))
    time.sleep(0.002)

u.UnhookWinEvent(hook)
time.sleep(0.5)

if not lat:
    print('没采到样本（超时 %d 次）' % misses[0])
    raise SystemExit(0)

lat.sort()


def pct(p):
    return lat[min(len(lat) - 1, int(len(lat) * p / 100.0))]


print('=== 切换窗口后，边框出现在新窗口上所需时间 ===')
print('  样本 %d 次，超时 %d 次' % (len(lat), misses[0]))
print('  中位数 %.0f ms   p90 %.0f ms   最慢 %.0f ms' % (pct(50), pct(90), lat[-1]))
for label, limit in (('一帧内(<=17ms)', 16.7), ('三帧内(<=50ms)', 50.0)):
    n = sum(1 for v in lat if v <= limit)
    print('  %-18s %d/%d (%.0f%%)' % (label, n, len(lat), 100.0 * n / len(lat)))
slow = sum(1 for v in lat if v > 100)
print('  超过 100ms（肉眼明显）：%d/%d' % (slow, len(lat)))
