# Overlay 模型下还值得查的只有两件事：每块显示器是不是都有一个 overlay，以及它在不在
# topmost 层的末尾。
#
# 旧模型那些「边框排在目标上面还是下面」「有没有孤立浮线」「边框窗口卡住了没有」的
# 检查全都没有意义了——边框不再是独立窗口，也不再参与 z 序竞争。
#
# 注意所有返回 HWND 的函数都显式声明了 restype：ctypes 默认按 32 位取返回值，而 HWND
# 是 64 位指针，不声明就会被悄悄截断——这个坑让之前几个诊断工具给出过假结论。
import ctypes
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND
u.GetTopWindow.restype = wintypes.HWND
u.GetTopWindow.argtypes = [wintypes.HWND]
u.GetWindow.restype = wintypes.HWND
u.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
u.GetWindowLongPtrW.restype = ctypes.c_longlong
u.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
WS_EX_TOPMOST = 0x8
GW_HWNDNEXT = 2
OVERLAY_CLASS = 'WindowMark.Overlay'


class MONITORINFO(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.DWORD), ('rcMonitor', wintypes.RECT),
                ('rcWork', wintypes.RECT), ('dwFlags', wintypes.DWORD)]


MONITORENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HANDLE, wintypes.HDC,
                                     ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)


def cls(h):
    b = ctypes.create_unicode_buffer(160)
    u.GetClassNameW(h, b, 160)
    return b.value


def rc(h):
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def enum(pred):
    out = []

    def cb(h, _):
        if pred(h):
            out.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    return out


def topmost(h):
    return (u.GetWindowLongPtrW(h, -20) & WS_EX_TOPMOST) != 0


def monitors():
    out = []

    def cb(mon, dc, rect, param):
        mi = MONITORINFO()
        mi.cbSize = ctypes.sizeof(mi)
        if u.GetMonitorInfoW(mon, ctypes.byref(mi)):
            out.append((mi.rcMonitor.left, mi.rcMonitor.top,
                        mi.rcMonitor.right, mi.rcMonitor.bottom))
        return True

    u.EnumDisplayMonitors(None, None, MONITORENUMPROC(cb), 0)
    return out


overlays = enum(lambda h: u.IsWindowVisible(h) and cls(h) == OVERLAY_CLASS)
mons = monitors()

print('显示器 %d 块，overlay %d 个' % (len(mons), len(overlays)))
print()

problems = 0
for m in mons:
    match = [o for o in overlays if rc(o) == m]
    if not match:
        print('  %-28s !! 没有对应的 overlay' % str(m))
        problems += 1
        continue
    o = match[0]
    print('  %-28s overlay 0x%-8X topmost=%s' % (str(m), o, topmost(o)))
    if not topmost(o):
        print('       !! 不在 topmost 层，会被普通窗口盖住')
        problems += 1

print()
# overlay 该落在两个 band 的交界：它下面紧邻的应该已经是普通层窗口了。
# 多显示器时几个 overlay 会在 topmost 层里挨着，那是自己人，跳过。
for o in overlays:
    below = u.GetWindow(o, GW_HWNDNEXT)
    for _ in range(4096):
        if not below:
            break
        if u.IsWindowVisible(below) and cls(below) != OVERLAY_CLASS:
            break
        below = u.GetWindow(below, GW_HWNDNEXT)
    if below and topmost(below):
        print('  !! overlay 0x%X 下面还压着 topmost 窗口 %s——它盖在别人上面了'
              % (o, cls(below)[:26]))
        problems += 1

if problems == 0:
    print('位置正确：每块显示器一个 overlay，都在 topmost 层的末尾')
else:
    print('问题 %d 处' % problems)
