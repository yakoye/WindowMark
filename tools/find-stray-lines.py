# 找「孤立的边框线」：屏幕上是边框色，却不落在任何被跟踪窗口的边框环上。
#
# check-occlusion.py 查的是「该裁没裁」，抓不到位置错的线（比如边框停在窗口的旧位置）。
# 这个反过来做：先在屏幕上把边框色的像素找出来，再问每一段「你属于谁的环」——没人认领
# 的就是残留或错位。
import ctypes
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
g = ctypes.WinDLL('gdi32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND
u.GetTopWindow.restype = wintypes.HWND
u.GetTopWindow.argtypes = [wintypes.HWND]
u.GetWindow.restype = wintypes.HWND
u.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

SRCCOPY = 0x00CC0020
GW_HWNDNEXT = 2
DWMWA_CLOAKED = 14
ACTIVE = (0x62, 0x74, 0xE7)
INACTIVE = (0x70, 0x80, 0xAA)
REACH = 3       # border.width + border.offset
SLACK = 4       # 环外再放宽几像素，圆角和抗锯齿会溢出一点


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


def cloaked(h):
    v = ctypes.c_int(0)
    if d.DwmGetWindowAttribute(wintypes.HWND(h), DWMWA_CLOAKED,
                               ctypes.byref(v), ctypes.sizeof(v)) != 0:
        return False
    return v.value != 0


def is_border(p, tol=14):
    for ref in (ACTIVE, INACTIVE):
        if all(abs(p[i] - ref[i]) <= tol for i in range(3)):
            return True
    return False


ox = u.GetSystemMetrics(76)
oy = u.GetSystemMetrics(77)
sw = u.GetSystemMetrics(78) or u.GetSystemMetrics(0)
sh = u.GetSystemMetrics(79) or u.GetSystemMetrics(1)

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

# 收窗口
wins = []
h = u.GetTopWindow(None)
while h:
    if u.IsWindowVisible(h) and not cls(h).startswith('WindowMark.') and not u.IsIconic(h):
        f = dwmr(h) or rc(h)
        if f[2] > f[0] and f[3] > f[1] and not cloaked(h):
            wins.append({'h': h, 'f': f})
    h = u.GetWindow(h, GW_HWNDNEXT)

fg = u.GetForegroundWindow()
print('前台 %s，窗口 %d 个，屏幕 %dx%d @(%d,%d)'
      % (pname(fg), len(wins), sw, sh, ox, oy))
print()


def on_any_ring(x, y):
    """这一点落在某个窗口的边框环上吗（放宽 SLACK）。"""
    for w in wins:
        f = w['f']
        outer = (f[0] - REACH - SLACK, f[1] - REACH - SLACK,
                 f[2] + REACH + SLACK, f[3] + REACH + SLACK)
        if not (outer[0] <= x < outer[2] and outer[1] <= y < outer[3]):
            continue
        inner = (f[0] + REACH + SLACK, f[1] + REACH + SLACK,
                 f[2] - REACH - SLACK, f[3] - REACH - SLACK)
        if inner[0] <= x < inner[2] and inner[1] <= y < inner[3]:
            continue      # 在环里面的洞里，不算环上
        return True
    return False


# 每 8 行扫一条水平线，把连续的边框色像素并成段
stray = []
for sy in range(0, sh, 8):
    run = None
    base = sy * sw * 4
    for sx in range(sw):
        off = base + sx * 4
        p = (raw[off + 2], raw[off + 1], raw[off])
        hit = is_border(p)
        if hit and run is None:
            run = [sx, sx]
        elif hit:
            run[1] = sx
        elif run is not None:
            if run[1] - run[0] >= 2:      # 太短的忽略，多半是别的界面元素
                mx = (run[0] + run[1]) // 2
                if not on_any_ring(mx + ox, sy + oy):
                    stray.append((run[0] + ox, sy + oy, run[1] + ox))
            run = None

if not stray:
    print('没有发现孤立的边框线。')
else:
    print('孤立线段 %d 处（屏幕上是边框色，却不属于任何窗口的边框环）：' % len(stray))
    # 把 x 接近的归到一起，便于看出是不是一整条竖线
    stray.sort()
    groups = {}
    for x0, y, x1 in stray:
        key = x0 // 16
        groups.setdefault(key, []).append((x0, y, x1))
    for key in sorted(groups):
        items = groups[key]
        ys = [it[1] for it in items]
        print('  x≈%-6d y 从 %d 到 %d，%d 段'
              % (items[0][0], min(ys), max(ys), len(items)))
        # 这一段最近的窗口边缘是谁
        near = []
        for w in wins:
            f = w['f']
            for edge, ex in (('左', f[0]), ('右', f[2])):
                if abs(ex - items[0][0]) <= 12:
                    near.append('%s的%s边(x=%d)' % (pname(w['h'])[:14], edge, ex))
        print('     附近的窗口边缘：%s' % ('  '.join(near[:4]) if near else '没有'))
