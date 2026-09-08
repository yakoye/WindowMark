# 逐像素看一个角画成了什么形状，判据只有一条：颜色是不是边框色。
#
# dump-corner 会把窗口内容也标出来，遇到背景色接近边框色的窗口整张图就糊了。这里只
# 认边框色，其余一律留白——形状是不是圆的、内侧有没有被咬掉一块，一眼就看得出来。
#
#   py tools\inspect-corner.py                前台窗口的左上角
#   py tools\inspect-corner.py chrome tr 24   chrome 的右上角，24x24
import ctypes
import sys
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
g = ctypes.WinDLL('gdi32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

SRCCOPY = 0x00CC0020
ACTIVE = (0x62, 0x74, 0xE7)
INACTIVE = (0x70, 0x80, 0xAA)
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


def find(keyword):
    out = []

    def cb(h, _):
        if u.IsWindowVisible(h) and not u.IsIconic(h) and not cls(h).startswith('WindowMark.'):
            r = frame(h)
            if (r[2] - r[0]) > 400 and (r[3] - r[1]) > 300:
                low = keyword.lower()
                if low in pname(h).lower() or low in cls(h).lower():
                    out.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    return out[0] if out else None


args = [a for a in sys.argv[1:]]
keyword = None
corner = 'tl'
size = 22
for a in args:
    if a in ('tl', 'tr', 'bl', 'br'):
        corner = a
    elif a.isdigit():
        size = int(a)
    else:
        keyword = a

target = find(keyword) if keyword else u.GetForegroundWindow()
if not target:
    print('找不到窗口')
    raise SystemExit(1)

f = frame(target)
print('%s (%s) %s   角=%s' % (pname(target), cls(target), str(f), corner))

# 角的起点：往窗口外留 6px，好看清边框的外沿。
PAD = 6
if corner in ('tl', 'bl'):
    x0 = f[0] - PAD
else:
    x0 = f[2] + PAD - size
if corner in ('tl', 'tr'):
    y0 = f[1] - PAD
else:
    y0 = f[3] + PAD - size

scr = u.GetDC(0)
md = g.CreateCompatibleDC(scr)
bm = g.CreateCompatibleBitmap(scr, size, size)
g.SelectObject(md, bm)
g.BitBlt(md, 0, 0, size, size, scr, x0, y0, SRCCOPY)
info = BITMAPINFO()
info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
info.bmiHeader.biWidth = size
info.bmiHeader.biHeight = -size
info.bmiHeader.biPlanes = 1
info.bmiHeader.biBitCount = 32
buf = ctypes.create_string_buffer(size * size * 4)
g.GetDIBits(md, bm, 0, size, buf, ctypes.byref(info), 0)
g.DeleteObject(bm)
g.DeleteDC(md)
u.ReleaseDC(0, scr)


def at(ix, iy):
    off = (iy * size + ix) * 4
    return (buf[off + 2][0], buf[off + 1][0], buf[off][0])


def grade(p):
    """'#' 实心边框色，'+' 抗锯齿过渡（掺了背景），' ' 完全不是。"""
    for ref in (ACTIVE, INACTIVE):
        diff = max(abs(p[i] - ref[i]) for i in range(3))
        if diff <= 10:
            return '#'
        if diff <= 70:
            return '+'
    return '.'


print("  '#' 实心边框色   '+' 抗锯齿过渡   '.' 别的东西")
print()
header = '     ' + ''.join(str((x0 + i) % 10) for i in range(size))
print(header)
for iy in range(size):
    row = ''.join(grade(at(ix, iy)) for ix in range(size))
    mark = ''
    if corner in ('tl', 'tr') and y0 + iy == f[1]:
        mark = ' <- 窗口上边界'
    if corner in ('bl', 'br') and y0 + iy == f[3]:
        mark = ' <- 窗口下边界'
    print('%4d %s%s' % (y0 + iy, row, mark))

edge = f[0] if corner in ('tl', 'bl') else f[2]
print()
print('窗口%s边界在 x=%d（图上第 %d 列）'
      % ('左' if corner in ('tl', 'bl') else '右', edge, edge - x0))
