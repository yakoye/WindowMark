# 前台窗口的边框该落在 topmost 层的**末尾**：在所有普通窗口之上，在所有 topmost
# 窗口（右键菜单、输入法候选框、任务栏、悬浮的会议小窗、别人置顶的窗口）之下。
#
# 这个脚本把边框上下相邻的窗口打出来，一眼看出它落在哪儿。
#
#   python tools\check-border-band.py
import ctypes
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
GW_HWNDNEXT, GW_HWNDPREV = 2, 3
WS_EX_TOPMOST = 0x8
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


def topmost(h):
    return (u.GetWindowLongPtrW(h, -20) & WS_EX_TOPMOST) != 0


def enum(pred):
    out = []

    def cb(h, _):
        if pred(h):
            out.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    return out


def label(h):
    return '%s(%s)%s' % (pname(h), cls(h)[:22], ' topmost' if topmost(h) else ' 普通层')


def neighbour(start, direction):
    h = start
    for _ in range(4096):
        h = u.GetWindow(h, direction)
        if not h:
            return None
        if u.IsWindowVisible(h) and not u.IsIconic(h):
            return h
    return None


fg = u.GetForegroundWindow()
f = dwm(fg) or rc(fg)
cx, cy = (f[0] + f[2]) / 2, (f[1] + f[3]) / 2
border, bestd = None, 10 ** 9
for b in enum(lambda h: u.IsWindowVisible(h) and cls(h) == BORDER_CLASS):
    r = rc(b)
    dist = abs((r[0] + r[2]) / 2 - cx) + abs((r[1] + r[3]) / 2 - cy)
    if dist < bestd:
        bestd, border = dist, b

print('前台窗口：%s' % label(fg))
if border is None or bestd > 80:
    print('前台窗口没有配对的边框（可能被排除，或边框功能关着）')
    raise SystemExit(0)

above = neighbour(border, GW_HWNDPREV)
below = neighbour(border, GW_HWNDNEXT)

print()
print('  上面紧邻：%s' % (label(above) if above else '（没有，边框在最前）'))
print('  边框    ：%s' % label(border))
print('  下面紧邻：%s' % (label(below) if below else '（没有，边框在最底）'))
print()

ok = True
if not topmost(border):
    print('!! 边框不在 topmost 层——会被普通窗口盖住')
    ok = False
if below is not None and topmost(below):
    print('!! 边框下面还有 topmost 窗口 %s——边框盖在它上面了' % pname(below))
    ok = False
if above is not None and not topmost(above):
    print('!! 边框上面压着的是普通层窗口 %s，不该出现' % pname(above))
    ok = False
if ok:
    print('位置正确：边框在 topmost 层的末尾')
    print('（普通窗口在它之下，菜单/候选框/任务栏/悬浮窗在它之上）')
