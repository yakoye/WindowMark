# 前台窗口的边框，画的是「活动色」还是「非活动色」？
#
# 这一条能把两类完全不同的故障分开：
#   画的是活动色 -> WindowMark 知道它是活动窗口，问题在 z 序那一段
#   画的是非活动色 -> WindowMark 根本不认为它是活动的，问题在事件/模型那一段
import ctypes
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
g = ctypes.WinDLL('gdi32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
SRCCOPY = 0x00CC0020
ACTIVE = (0x62, 0x74, 0xE7)
INACTIVE = (0x70, 0x80, 0xAA)
BORDER_CLASS = 'WindowMark.WindowBorder'


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
        # create_string_buffer 的索引返回单字节 bytes，不是 int，要再取一次 [0]。
        off = (iy * w + ix) * 4
        return (buf[off + 2][0], buf[off + 1][0], buf[off][0])

    return px


def near(p, ref, tol=40):
    return all(abs(p[i] - ref[i]) <= tol for i in range(3))


fg = u.GetForegroundWindow()
f = dwm(fg) or rc(fg)
print('前台：%s (%s)  DWM%s' % (pname(fg), cls(fg), str(f)))

pool = enum(lambda h: u.IsWindowVisible(h) and cls(h) == BORDER_CLASS)
cx, cy = (f[0] + f[2]) / 2, (f[1] + f[3]) / 2
border, bestd = None, 10 ** 9
for b in pool:
    r = rc(b)
    dist = abs((r[0] + r[2]) / 2 - cx) + abs((r[1] + r[3]) / 2 - cy)
    if dist < bestd:
        bestd, border = dist, b
if border is None or bestd > 80:
    print('前台窗口没有配对的边框（最近的差 %d px）' % bestd)
    raise SystemExit(0)

print('边框：0x%X  矩形%s' % (border, str(rc(border))))
print()

# 沿窗口外沿 2px 处取样，四条边各 5 点
ring = 2
fr = [(0.15, 0.35, 0.5, 0.65, 0.85)]
pts = []
for t in fr[0]:
    pts.append(('上', f[0] + int((f[2] - f[0]) * t), f[1] - ring))
    pts.append(('下', f[0] + int((f[2] - f[0]) * t), f[3] + ring - 1))
    pts.append(('左', f[0] - ring, f[1] + int((f[3] - f[1]) * t)))
    pts.append(('右', f[2] + ring - 1, f[1] + int((f[3] - f[1]) * t)))

x0 = min(p[1] for p in pts) - 2
y0 = min(p[2] for p in pts) - 2
x1 = max(p[1] for p in pts) + 3
y1 = max(p[2] for p in pts) + 3
px = grab(x0, y0, x1 - x0, y1 - y0)

act = inact = other = 0
samples = []
for name, sx, sy in pts:
    c = px(sx - x0, sy - y0)
    if near(c, ACTIVE):
        act += 1
    elif near(c, INACTIVE):
        inact += 1
    else:
        other += 1
        if len(samples) < 4:
            samples.append('%s边(%d,%d)=%s' % (name, sx, sy, str(c)))

print('活动色 #6274E7 命中 %d 点' % act)
print('非活动色 #7080AA 命中 %d 点' % inact)
print('都不是 %d 点  %s' % (other, '  '.join(samples)))
print()
if act > inact and act > 0:
    print('=> 画的是活动色：WindowMark 知道它是活动窗口，问题在 z 序那一段。')
elif inact > 0:
    print('=> 画的是非活动色：WindowMark 不认为它是活动窗口，问题在事件/模型那一段。')
else:
    print('=> 一个边框像素都没采到：边框整个被别的窗口盖住了。')
