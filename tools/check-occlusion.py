# 查「本该被遮住的边框却画出来了」。
#
# Overlay 模型下这是唯一还可能出错的地方：z 序读错、快照过期、或者某个窗口没算进
# 遮挡物。做法是自己按 z 序算一遍「这一点该不该有边框」，再去屏幕上取像素对照。
#
#   python tools\check-occlusion.py
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
u.GetWindowLongPtrW.restype = ctypes.c_longlong
u.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

SRCCOPY = 0x00CC0020
GW_HWNDNEXT = 2
DWMWA_CLOAKED = 14
ACTIVE = (0x62, 0x74, 0xE7)
INACTIVE = (0x70, 0x80, 0xAA)
REACH = 3          # border.width + border.offset


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


def grab(x, y, w, h):
    screen = u.GetDC(0)
    mem = g.CreateCompatibleDC(screen)
    bmp = g.CreateCompatibleBitmap(screen, w, h)
    g.SelectObject(mem, bmp)
    g.BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY)
    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    g.DeleteObject(bmp)
    g.DeleteDC(mem)
    u.ReleaseDC(0, screen)

    def px(ix, iy):
        if ix < 0 or iy < 0 or ix >= w or iy >= h:
            return None
        off = (iy * w + ix) * 4
        return (buf[off + 2][0], buf[off + 1][0], buf[off][0])

    return px


# 容差收到 12。边框是纯色填的，屏幕上取到的应该几乎一模一样；放宽到 40 会把壁纸和
# 应用界面里偏蓝的像素也算进来，之前就误报过。
def is_border(p, tol=12):
    if p is None:
        return False
    for ref in (ACTIVE, INACTIVE):
        if all(abs(p[i] - ref[i]) <= tol for i in range(3)):
            return True
    return False


def inside(pt, r):
    return r[0] <= pt[0] < r[2] and r[1] <= pt[1] < r[3]


# 按 z 序从上到下收窗口，口径和 CaptureDesktop 一致
order = []
h = u.GetTopWindow(None)
while h:
    if u.IsWindowVisible(h) and not cls(h).startswith('WindowMark.'):
        f = dwmr(h) or rc(h)
        if f[2] > f[0] and f[3] > f[1]:
            order.append({'h': h, 'f': f, 'cloaked': cloaked(h),
                          'min': bool(u.IsIconic(h)), 'max': bool(u.IsZoomed(h))})
    h = u.GetWindow(h, GW_HWNDNEXT)

fg = u.GetForegroundWindow()
print('前台 %s (%s)' % (pname(fg), cls(fg)))
print('z 序里的可见窗口 %d 个' % len(order))
print()

# 一屏抓一次，后面所有采样都从这张图取，免得中途画面变了对不上
sw = u.GetSystemMetrics(78) or u.GetSystemMetrics(0)   # SM_CXVIRTUALSCREEN
sh = u.GetSystemMetrics(79) or u.GetSystemMetrics(1)
ox = u.GetSystemMetrics(76)                            # SM_XVIRTUALSCREEN
oy = u.GetSystemMetrics(77)
px = grab(ox, oy, sw, sh)

problems = 0
for i, w in enumerate(order):
    if w['cloaked'] or w['min'] or w['max']:
        continue
    f = w['f']
    if (f[2] - f[0]) < 300 or (f[3] - f[1]) < 200:
        continue
    above = [o['f'] for o in order[:i] if not o['cloaked'] and not o['min']]

    # 沿边框环取样：四条边各 9 点，落在窗口外沿 2px 处
    ring = 2
    pts = []
    for t in [x / 40.0 for x in range(1, 40)]:      # 每条边 39 点，够密才看得出是不是一整条
        pts.append((f[0] + int((f[2] - f[0]) * t), f[1] - ring))
        pts.append((f[0] + int((f[2] - f[0]) * t), f[3] + ring - 1))
        pts.append((f[0] - ring, f[1] + int((f[3] - f[1]) * t)))
        pts.append((f[2] + ring - 1, f[1] + int((f[3] - f[1]) * t)))

    wrong = []
    for pt in pts:
        covered = any(inside(pt, a) for a in above)
        c = px(pt[0] - ox, pt[1] - oy)
        # 被上面的窗口盖住了，却还画着边框 —— 这就是要找的错
        if covered and is_border(c):
            wrong.append((pt, c))

    if wrong:
        problems += 1
        blockers = set()
        for pt, _ in wrong[:8]:
            for j, o in enumerate(order[:i]):
                if not o['cloaked'] and not o['min'] and inside(pt, o['f']):
                    blockers.add('%s(z=%d) %s' % (pname(o['h'])[:14], j, str(o['f'])))
                    break
        print('!! %s (z=%d) %s' % (pname(w['h'])[:16], i, str(f)))
        print('   %d/%d 个采样点被盖住却仍画着边框' % (len(wrong), len(pts)))
        for b in sorted(blockers):
            print('   压着它的：%s' % b)
        for pt, c in wrong[:5]:
            print('   点 %-16s 实测颜色 %s' % (str(pt), str(c)))
        print()

if problems == 0:
    print('没有发现「被盖住却仍画着边框」的窗口。')
else:
    print('有问题的窗口 %d 个' % problems)
