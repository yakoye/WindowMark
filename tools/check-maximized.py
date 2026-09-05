# 最大化的窗口不该有边框，非最大化的该有。逐个列出来对照。
#
# 注意所有返回 HWND 的函数都显式声明了 restype：ctypes 默认按 32 位取返回值，而
# HWND 是 64 位指针，不声明就会被悄悄截断——这个坑让之前几个诊断工具给出过假结论。
import ctypes
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)

u.GetWindow.restype = wintypes.HWND
u.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
u.GetForegroundWindow.restype = wintypes.HWND
u.GetWindowLongPtrW.restype = ctypes.c_longlong
u.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
BORDER_CLASS = 'WindowMark.WindowBorder'


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


def dwm(h):
    r = wintypes.RECT()
    if d.DwmGetWindowAttribute(wintypes.HWND(h), 9, ctypes.byref(r), ctypes.sizeof(r)) != 0:
        return None
    return (r.left, r.top, r.right, r.bottom)


def enum(pred):
    out = []

    def cb(h, _):
        if pred(h):
            out.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    return out


borders = enum(lambda h: u.IsWindowVisible(h) and cls(h) == BORDER_CLASS)


def has_border(h):
    f = dwm(h) or rc(h)
    cx, cy = (f[0] + f[2]) / 2, (f[1] + f[3]) / 2
    for b in borders:
        r = rc(b)
        if abs((r[0] + r[2]) / 2 - cx) + abs((r[1] + r[3]) / 2 - cy) <= 40:
            return True
    return False


fg = u.GetForegroundWindow()
print('边框窗口共 %d 个' % len(borders))
print()
print('%-20s %-26s %-8s %-8s %s' % ('进程', '类名', '最大化', '有边框', '判定'))
print('-' * 82)

bad = 0
for h in enum(lambda x: u.IsWindowVisible(x) and not u.IsIconic(x)):
    c = cls(h)
    if c.startswith('WindowMark.'):
        continue
    r = rc(h)
    if (r[2] - r[0]) < 300 or (r[3] - r[1]) < 200:
        continue
    zoomed = u.IsZoomed(h) != 0
    got = has_border(h)
    if zoomed and got:
        verdict = '!! 最大化却有边框'
        bad += 1
    elif zoomed:
        verdict = 'OK（最大化，无边框）'
    elif got:
        verdict = 'OK'
    else:
        verdict = '（没边框，可能被排除或不合资格）'
    print('%-20s %-26s %-8s %-8s %s%s'
          % (pname(h)[:20], c[:26], '是' if zoomed else '否',
             '有' if got else '无', verdict, '  <<< 前台' if h == fg else ''))

print()
print('最大化却仍有边框的：%d 个' % bad)
