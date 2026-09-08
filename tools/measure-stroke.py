# 量前台窗口边框的实际线宽：四条边各九个位置，四个圆角沿半径方向量。
#
# 圆角必须沿**半径方向**量。沿水平或垂直方向量弧，量到的是斜边，天然比线宽大或小，
# 不能拿来和直边比。
#
# 同时用两档容差：严格的量「实心部分」，宽松的量「含抗锯齿边缘的总宽」。圆角是 D2D
# 画的、带抗锯齿，直边是像素填充、没有——两者的差值能看出边缘过渡有多少。
import ctypes
import math
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


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG),
                ('biHeight', wintypes.LONG), ('biPlanes', wintypes.WORD),
                ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD),
                ('biSizeImage', wintypes.DWORD), ('biXPelsPerMeter', wintypes.LONG),
                ('biYPelsPerMeter', wintypes.LONG), ('biClrUsed', wintypes.DWORD),
                ('biClrImportant', wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [('bmiHeader', BITMAPINFOHEADER), ('bmiColors', wintypes.DWORD * 3)]


class MONITORINFO(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.DWORD), ('rcMonitor', wintypes.RECT),
                ('rcWork', wintypes.RECT), ('dwFlags', wintypes.DWORD)]


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


# 整屏抓一次，所有采样都从这张图取
ox = u.GetSystemMetrics(76)
oy = u.GetSystemMetrics(77)
sw = u.GetSystemMetrics(78) or u.GetSystemMetrics(0)
sh = u.GetSystemMetrics(79) or u.GetSystemMetrics(1)


def grab_screen():
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
    b = ctypes.create_string_buffer(sw * sh * 4)
    g.GetDIBits(md, bm, 0, sh, b, ctypes.byref(info), 0)
    g.DeleteObject(bm)
    g.DeleteDC(md)
    u.ReleaseDC(0, scr)
    return bytes(b)


screen = u.GetDC(0)
mem = g.CreateCompatibleDC(screen)
bmp = g.CreateCompatibleBitmap(screen, sw, sh)
g.SelectObject(mem, bmp)
g.BitBlt(mem, 0, 0, sw, sh, screen, ox, oy, SRCCOPY)
bi = BITMAPINFO()
bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
bi.bmiHeader.biWidth = sw
bi.bmiHeader.biHeight = -sh
bi.bmiHeader.biPlanes = 1
bi.bmiHeader.biBitCount = 32
buf = ctypes.create_string_buffer(sw * sh * 4)
g.GetDIBits(mem, bmp, 0, sh, buf, ctypes.byref(bi), 0)
g.DeleteObject(bmp)
g.DeleteDC(mem)
u.ReleaseDC(0, screen)
raw = bytes(buf)


def at(x, y):
    ix, iy = x - ox, y - oy
    if ix < 0 or iy < 0 or ix >= sw or iy >= sh:
        return None
    off = (iy * sw + ix) * 4
    return (raw[off + 2], raw[off + 1], raw[off])


def hit(x, y, tol):
    p = at(x, y)
    if p is None:
        return False
    for ref in (ACTIVE, INACTIVE):
        if all(abs(p[i] - ref[i]) <= tol for i in range(3)):
            return True
    return False


def scan(cx, cy, dx, dy, span, tol):
    """从 (cx,cy) 沿 (dx,dy) 两侧各扫 span 步，数命中多少。"""
    n = 0
    for s in range(-span, span + 1):
        x = int(round(cx + dx * s))
        y = int(round(cy + dy * s))
        if hit(x, y, tol):
            n += 1
    return n


import sys

P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def find_window(keyword):
    """按进程名或类名的关键字找一个够大的可见窗口。"""
    found = []

    def cb(h, _):
        if not u.IsWindowVisible(h) or u.IsIconic(h):
            return True
        if cls(h).startswith('WindowMark.'):
            return True
        r = rc(h)
        if (r[2] - r[0]) < 300 or (r[3] - r[1]) < 200:
            return True
        low = keyword.lower()
        if low in pname(h).lower() or low in cls(h).lower():
            found.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    return found[0] if found else None


if len(sys.argv) > 1:
    fg = find_window(sys.argv[1])
    if not fg:
        print('找不到匹配 "%s" 的可见窗口' % sys.argv[1])
        raise SystemExit(1)
else:
    fg = u.GetForegroundWindow()

# 抓屏前后各取一次窗口矩形，不一致就重来。
#
# 窗口在抓屏和取矩形之间移动过的话，采样点会整体错位——之前就因为这个把一条完好的
# 右边框量成了「0 px」，白查了一轮。
f = None
for attempt in range(8):
    before = dwmr(fg) or rc(fg)
    raw = grab_screen()
    after = dwmr(fg) or rc(fg)
    if before == after:
        f = before
        break
    import time as _t
    _t.sleep(0.15)
if f is None:
    print('窗口一直在动，量不准。等它停下来再跑。')
    raise SystemExit(1)

mon = u.MonitorFromWindow(fg, 2)
mi = MONITORINFO()
mi.cbSize = ctypes.sizeof(mi)
u.GetMonitorInfoW(mon, ctypes.byref(mi))
m = (mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom)

print('前台 %s (%s)' % (pname(fg), cls(fg)))
print('DWM 边界 %s' % str(f))
print('离显示器四边：左 %d  上 %d  右 %d  下 %d'
      % (f[0] - m[0], f[1] - m[1], m[2] - f[2], m[3] - f[3]))
print()

SPAN = 16
FRACS = [x / 10.0 for x in range(1, 10)]      # 9 个位置

print('%-6s %-28s %s' % ('', '严格(tol=10) 九个位置', '宽松(tol=40)'))
print('-' * 62)
edges = {}
for name, gen, d_ in (
        ('上', lambda t: (f[0] + int((f[2] - f[0]) * t), f[1]), (0, 1)),
        ('下', lambda t: (f[0] + int((f[2] - f[0]) * t), f[3]), (0, 1)),
        ('左', lambda t: (f[0], f[1] + int((f[3] - f[1]) * t)), (1, 0)),
        ('右', lambda t: (f[2], f[1] + int((f[3] - f[1]) * t)), (1, 0))):
    strict = [scan(*gen(t), d_[0], d_[1], SPAN, 10) for t in FRACS]
    loose = [scan(*gen(t), d_[0], d_[1], SPAN, 40) for t in FRACS]
    edges[name] = strict
    print('%-6s %-28s %s'
          % (name + '边', ' '.join(str(v) for v in strict),
             ' '.join(str(v) for v in loose)))

print()
print('四个圆角，沿半径方向量（45 度斜着扫）：')
diag = 1.0 / math.sqrt(2.0)
R = 12      # 只用来定位取样点，不影响测量
for name, cx, cy, dx, dy in (
        ('左上', f[0] + R, f[1] + R, -diag, -diag),
        ('右上', f[2] - R, f[1] + R, diag, -diag),
        ('左下', f[0] + R, f[3] - R, -diag, diag),
        ('右下', f[2] - R, f[3] - R, diag, diag)):
    strict = scan(cx, cy, dx, dy, SPAN, 10)
    loose = scan(cx, cy, dx, dy, SPAN, 40)
    print('  %s角  严格 %d px   宽松 %d px' % (name, strict, loose))

print()
vals = [v for e in edges.values() for v in e]
if len(set(vals)) == 1:
    print('四条边严格口径完全一致：%d px' % vals[0])
else:
    print('四条边严格口径不一致，出现过的值：%s' % str(sorted(set(vals))))
    for name in ('上', '下', '左', '右'):
        s = set(edges[name])
        print('   %s边 %s' % (name, str(sorted(s))))
