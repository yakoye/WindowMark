# 持续盯着屏幕，一旦出现「孤立的边框线」就记下当时的现场。
#
# 这类线是瞬时的：切换窗口时闪一下，等你回头去查已经没了。所以必须连续扫，抓到就
# 立刻把当时的窗口列表、前台、线的位置一起存下来。
#
#   python tools\watch-stray.py 30      盯 30 秒
import ctypes
import sys
import time
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
REACH = 3
SLACK = 5
STEP = 24        # 每隔多少行扫一条；只要线够长就一定会被扫到


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


def is_border(p):
    for ref in (ACTIVE, INACTIVE):
        if all(abs(p[i] - ref[i]) <= 10 for i in range(3)):
            return True, ref is ACTIVE
    return False, False


ox = u.GetSystemMetrics(76)
oy = u.GetSystemMetrics(77)
sw = u.GetSystemMetrics(78) or u.GetSystemMetrics(0)
sh = u.GetSystemMetrics(79) or u.GetSystemMetrics(1)


def snap_windows():
    out = []
    h = u.GetTopWindow(None)
    while h:
        if u.IsWindowVisible(h) and not cls(h).startswith('WindowMark.') and not u.IsIconic(h):
            f = dwmr(h) or rc(h)
            if f[2] > f[0] and f[3] > f[1] and not cloaked(h):
                out.append({'h': h, 'f': f, 'name': pname(h), 'cls': cls(h)})
        h = u.GetWindow(h, GW_HWNDNEXT)
    return out


def grab_rows(rows):
    """只抓需要的那几行，比整屏快得多。"""
    screen = u.GetDC(0)
    mem = g.CreateCompatibleDC(screen)
    bmp = g.CreateCompatibleBitmap(screen, sw, len(rows))
    g.SelectObject(mem, bmp)
    for i, y in enumerate(rows):
        g.BitBlt(mem, 0, i, sw, 1, screen, ox, y, SRCCOPY)
    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = sw
    bi.bmiHeader.biHeight = -len(rows)
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(sw * len(rows) * 4)
    g.GetDIBits(mem, bmp, 0, len(rows), buf, ctypes.byref(bi), 0)
    g.DeleteObject(bmp)
    g.DeleteDC(mem)
    u.ReleaseDC(0, screen)
    return bytes(buf)


def on_ring(x, y, wins):
    for w in wins:
        f = w['f']
        if not (f[0] - REACH - SLACK <= x < f[2] + REACH + SLACK):
            continue
        if not (f[1] - REACH - SLACK <= y < f[3] + REACH + SLACK):
            continue
        if (f[0] + REACH + SLACK <= x < f[2] - REACH - SLACK and
                f[1] + REACH + SLACK <= y < f[3] - REACH - SLACK):
            continue
        return True
    return False


seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 30
rows = list(range(oy + STEP // 2, oy + sh, STEP))
print('盯 %d 秒，扫 %d 行（每 %d 行一条）。请复现那条线。' % (seconds, len(rows), STEP))
print()

end = time.time() + seconds
hits = 0
seen = set()
while time.time() < end:
    wins = snap_windows()
    fg = u.GetForegroundWindow()
    raw = grab_rows(rows)

    stray = []
    for i, y in enumerate(rows):
        base = i * sw * 4
        run = None
        for x in range(sw):
            off = base + x * 4
            hit, _ = is_border((raw[off + 2], raw[off + 1], raw[off]))
            if hit and run is None:
                run = [x, x]
            elif hit:
                run[1] = x
            elif run is not None:
                if run[1] - run[0] >= 1:
                    mx = (run[0] + run[1]) // 2 + ox
                    if not on_ring(mx, y, wins):
                        stray.append((mx, y, run[1] - run[0] + 1))
                run = None

    # 同一 x 上出现多段才算「一条线」，单点忽略
    cols = {}
    for x, y, wide in stray:
        cols.setdefault(x // 8, []).append((x, y, wide))
    lines = [v for v in cols.values() if len(v) >= 3]

    if lines:
        hits += 1
        for items in lines:
            key = items[0][0] // 8
            if key in seen:
                continue
            seen.add(key)
            ys = [it[1] for it in items]
            print('[%s] 孤立线 x≈%d  y %d..%d  %d 段  宽 %d px'
                  % (time.strftime('%H:%M:%S'), items[0][0], min(ys), max(ys),
                     len(items), items[0][2]))
            print('     前台：%s (%s) %s' % (pname(fg), cls(fg), str(rc(fg))))
            print('     那一列上的窗口（按 z 序）：')
            for w in wins:
                f = w['f']
                if f[0] - 8 <= items[0][0] <= f[2] + 8 and f[1] <= min(ys) <= f[3]:
                    edge = ''
                    if abs(f[0] - items[0][0]) <= 8:
                        edge = '  <- 左边缘'
                    elif abs(f[2] - items[0][0]) <= 8:
                        edge = '  <- 右边缘'
                    print('       %-18s %-24s %s%s'
                          % (w['name'][:18], w['cls'][:24], str(f), edge))
            print()
    time.sleep(0.12)

print()
if hits == 0:
    print('这段时间里没有抓到孤立线。')
else:
    print('抓到 %d 次采样里有孤立线，去重后 %d 条。' % (hits, len(seen)))
