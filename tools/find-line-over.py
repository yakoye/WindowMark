# 找出穿过某个窗口内部的边框线，并推断它是谁的边框。
#
# 「对话框被边框线切了」这句话里，最要紧的信息是那条线的坐标——它对上谁的窗口边缘，
# 就是谁的边框跑到了不该去的地方。肉眼看截图给不出这个数。
#
#   py tools\find-line-over.py mobaxterm TFormParams
#   py tools\find-line-over.py                        前台窗口
import ctypes
import sys
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
g = ctypes.WinDLL('gdi32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND
u.GetTopWindow.restype = wintypes.HWND
u.GetWindow.restype = wintypes.HWND
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

SRCCOPY = 0x00CC0020
ACTIVE = (0x62, 0x74, 0xE7)
INACTIVE = (0x70, 0x80, 0xAA)
GW_HWNDNEXT = 2
P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


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


args = sys.argv[1:]
target = None
if args:
    want_proc = args[0].lower()
    want_cls = args[1].lower() if len(args) > 1 else None
    found = []

    def cb(h, _):
        if u.IsWindowVisible(h) and not u.IsIconic(h) and not cls(h).startswith('WindowMark.'):
            if want_proc in pname(h).lower() or want_proc in cls(h).lower():
                if want_cls is None or want_cls in cls(h).lower():
                    found.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    if found:
        target = found[0]
else:
    target = u.GetForegroundWindow()

if not target:
    print('找不到窗口')
    raise SystemExit(1)

f = frame(target)
print('目标 %s (%s) %s' % (pname(target), cls(target), str(f)))
print('前台 %s' % pname(u.GetForegroundWindow()))
print()

# 只看窗口内部，边缘 8px 留给它自己的边框
MARGIN = 8
x0, y0 = f[0] + MARGIN, f[1] + MARGIN
w, h = f[2] - f[0] - MARGIN * 2, f[3] - f[1] - MARGIN * 2
if w <= 0 or h <= 0:
    print('窗口太小')
    raise SystemExit(1)

scr = u.GetDC(0)
md = g.CreateCompatibleDC(scr)
bm = g.CreateCompatibleBitmap(scr, w, h)
g.SelectObject(md, bm)
g.BitBlt(md, 0, 0, w, h, scr, x0, y0, SRCCOPY)
info = BITMAPINFO()
info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
info.bmiHeader.biWidth = w
info.bmiHeader.biHeight = -h
info.bmiHeader.biPlanes = 1
info.bmiHeader.biBitCount = 32
buf = ctypes.create_string_buffer(w * h * 4)
g.GetDIBits(md, bm, 0, h, buf, ctypes.byref(info), 0)
g.DeleteObject(bm)
g.DeleteDC(md)
u.ReleaseDC(0, scr)


def hit(ix, iy):
    off = (iy * w + ix) * 4
    p = (buf[off + 2][0], buf[off + 1][0], buf[off][0])
    return any(all(abs(p[i] - ref[i]) <= 12 for i in range(3))
               for ref in (ACTIVE, INACTIVE))


# 竖线：某一列上连续命中很长
MIN_RUN = 40
verticals = []
for ix in range(w):
    run = 0
    best = 0
    for iy in range(h):
        run = run + 1 if hit(ix, iy) else 0
        best = max(best, run)
    if best >= MIN_RUN:
        verticals.append((x0 + ix, best))

horizontals = []
for iy in range(h):
    run = 0
    best = 0
    for ix in range(w):
        run = run + 1 if hit(ix, iy) else 0
        best = max(best, run)
    if best >= MIN_RUN:
        horizontals.append((y0 + iy, best))

# 桌面上所有可见窗口的边缘，用来认领这些线
edges = []
hwnd = u.GetTopWindow(None)
guard = 0
while hwnd and guard < 4096:
    guard += 1
    if u.IsWindowVisible(hwnd) and not cls(hwnd).startswith('WindowMark.') and not cloaked(hwnd):
        r = frame(hwnd)
        if r[2] > r[0] and r[3] > r[1]:
            edges.append((pname(hwnd), cls(hwnd), r, int(hwnd) == int(target)))
    hwnd = u.GetWindow(hwnd, GW_HWNDNEXT)


def claim_vertical(x):
    out = []
    for proc, c, r, is_self in edges:
        for name, ex in (('左', r[0]), ('右', r[2])):
            if abs(x - ex) <= 4:
                out.append('%s(%s)的%s边 x=%d%s'
                           % (proc, c[:18], name, ex, '  ←就是它自己' if is_self else ''))
    return out


def claim_horizontal(y):
    out = []
    for proc, c, r, is_self in edges:
        for name, ey in (('上', r[1]), ('下', r[3])):
            if abs(y - ey) <= 4:
                out.append('%s(%s)的%s边 y=%d%s'
                           % (proc, c[:18], name, ey, '  ←就是它自己' if is_self else ''))
    return out


if not verticals and not horizontals:
    print('窗口内部没有找到边框色的长线段。')
else:
    print('窗口内部找到的边框线（内缩 %d px 之后还在的，就是穿进去了）：' % MARGIN)
    print()
    for x, run in verticals:
        print('  竖线 x=%d，最长连续 %d px' % (x, run))
        for who in claim_vertical(x) or ['（对不上任何窗口边缘）']:
            print('      %s' % who)
    for y, run in horizontals:
        print('  横线 y=%d，最长连续 %d px' % (y, run))
        for who in claim_horizontal(y) or ['（对不上任何窗口边缘）']:
            print('      %s' % who)
