# 最大化的窗口不该有边框，非最大化的该有。
#
# 判据是**屏幕上的像素**，不是「有没有配对的边框窗口」。overlay 模型下边框不再是一个
# 个独立窗口，它们全画在每块显示器一张画布上——问「这个窗口有没有自己的边框窗口」已经
# 没有意义了，只能去看那几条线到底画出来没有。
#
# 最大化的窗口为什么不该有：它贴着工作区边缘，外面没有画边框的那几像素；夹回屏幕内
# 之后边框和窗口边界完全重合，四条边一个像素都露不出来。置顶窗口例外——置顶是显式
# 操作，边框是它生效的唯一视觉反馈。
#
#   py tools\check-maximized.py
import ctypes
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
g = ctypes.WinDLL('gdi32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)

# 所有返回 HWND 的函数都显式声明 restype：ctypes 默认按 32 位取返回值，而 HWND 是
# 64 位指针，不声明就会被悄悄截断——这个坑让之前几个诊断工具给出过假结论。
u.GetWindow.restype = wintypes.HWND
u.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
u.GetForegroundWindow.restype = wintypes.HWND
u.GetTopWindow.restype = wintypes.HWND
u.GetWindowLongPtrW.restype = ctypes.c_longlong
u.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

SRCCOPY = 0x00CC0020
ACTIVE = (0x62, 0x74, 0xE7)
INACTIVE = (0x70, 0x80, 0xAA)
GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x8
P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

# 桌面和任务栏本来就没有边框，列进来只会一律报「该有却没有」。
SHELL_CLASSES = ('Progman', 'WorkerW', 'Shell_TrayWnd', 'Shell_SecondaryTrayWnd',
                 'TopLevelWindowForOverflowXamlIsland')


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG),
                ('biHeight', wintypes.LONG), ('biPlanes', wintypes.WORD),
                ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD),
                ('biSizeImage', wintypes.DWORD), ('biXPelsPerMeter', wintypes.LONG),
                ('biYPelsPerMeter', wintypes.LONG), ('biClrUsed', wintypes.DWORD),
                ('biClrImportant', wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [('bmiHeader', BITMAPINFOHEADER), ('bmiColors', wintypes.DWORD * 3)]


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
    n = wintypes.DWORD(260)
    ok = k.QueryFullProcessImageNameW(hp, 0, b, ctypes.byref(n))
    k.CloseHandle(hp)
    return b.value.rsplit('\\', 1)[-1] if ok else '?'


def frame(h):
    r = wintypes.RECT()
    if d.DwmGetWindowAttribute(wintypes.HWND(h), 9, ctypes.byref(r), ctypes.sizeof(r)) != 0:
        u.GetWindowRect(wintypes.HWND(h), ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def cloaked(h):
    v = ctypes.c_int(0)
    d.DwmGetWindowAttribute(wintypes.HWND(h), 14, ctypes.byref(v), ctypes.sizeof(v))
    return v.value != 0


ox = u.GetSystemMetrics(76)
oy = u.GetSystemMetrics(77)
sw = u.GetSystemMetrics(78) or u.GetSystemMetrics(0)
sh = u.GetSystemMetrics(79) or u.GetSystemMetrics(1)

scr = u.GetDC(0)
md = g.CreateCompatibleDC(scr)
bm = g.CreateCompatibleBitmap(scr, sw, sh)
g.SelectObject(md, bm)
g.BitBlt(md, 0, 0, sw, sh, scr, ox, oy, SRCCOPY)
info = BITMAPINFO()
info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
info.bmiHeader.biWidth = sw
info.bmiHeader.biHeight = -sh
info.bmiHeader.biPlanes = 1
info.bmiHeader.biBitCount = 32
buf = ctypes.create_string_buffer(sw * sh * 4)
g.GetDIBits(md, bm, 0, sh, buf, ctypes.byref(info), 0)
g.DeleteObject(bm)
g.DeleteDC(md)
u.ReleaseDC(0, scr)
raw = bytes(buf)


def at(x, y):
    ix, iy = x - ox, y - oy
    if ix < 0 or iy < 0 or ix >= sw or iy >= sh:
        return None
    off = (iy * sw + ix) * 4
    return (raw[off + 2], raw[off + 1], raw[off])


def border_at(x, y, horizontal, span=10):
    for s in range(-span, span + 1):
        p = at(x, y + s) if horizontal else at(x + s, y)
        if p is None:
            continue
        if any(all(abs(p[i] - ref[i]) <= 12 for i in range(3)) for ref in (ACTIVE, INACTIVE)):
            return True
    return False


def border_hits(f):
    """四条边各取三个位置，数命中几个（满分 12）。"""
    n = 0
    for t in (0.3, 0.5, 0.7):
        x = f[0] + int((f[2] - f[0]) * t)
        y = f[1] + int((f[3] - f[1]) * t)
        n += border_at(x, f[1], True)
        n += border_at(x, f[3], True)
        n += border_at(f[0], y, False)
        n += border_at(f[2], y, False)
    return n


rows = []
hwnd = u.GetTopWindow(None)
guard = 0
while hwnd and guard < 4096:
    guard += 1
    if u.IsWindowVisible(hwnd) and not u.IsIconic(hwnd) \
            and not cls(hwnd).startswith('WindowMark.') \
            and not cloaked(hwnd) \
            and cls(hwnd) not in SHELL_CLASSES:
        f = frame(hwnd)
        if f[2] - f[0] > 300 and f[3] - f[1] > 200:
            ex = u.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
            rows.append((int(hwnd), pname(hwnd), cls(hwnd), f,
                         u.IsZoomed(hwnd) != 0, bool(ex & WS_EX_TOPMOST)))
    hwnd = u.GetWindow(hwnd, 2)

if not rows:
    print('屏幕上没有够大的可见窗口。')
    raise SystemExit(0)

print('%-22s %-26s %-8s %-8s %s' % ('进程', '类名', '最大化', '置顶', '边框命中(满分12)'))
print('-' * 88)
bad = []
for hwnd, proc, name, f, zoomed, topmost in rows:
    hits = border_hits(f)
    want = '不该有' if (zoomed and not topmost) else '该有'
    # 被别的窗口盖住的部分本来就不画，所以「该有」只要求见到一些，不要求满分。
    wrong = (zoomed and not topmost and hits > 2) or (not zoomed and hits == 0)
    print('%-22s %-26s %-8s %-8s %2d   %s%s'
          % (proc[:22], name[:26], '是' if zoomed else '否', '是' if topmost else '否',
             hits, want, '   <<< 对不上' if wrong else ''))
    if wrong:
        bad.append((proc, name, zoomed, hits))

print()
if not bad:
    print('都对得上。')
else:
    for proc, name, zoomed, hits in bad:
        if zoomed:
            print('%s (%s) 最大化了却还有边框（命中 %d）' % (proc, name, hits))
        else:
            print('%s (%s) 没最大化却一条边框都没有——也可能是被别的窗口整个盖住了，'
                  '或者它在排除名单里' % (proc, name))
