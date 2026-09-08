# 把前台窗口左上角那一小块的像素打出来，看边框在角上到底画成什么形状。
#
# 截图看不清一两像素的事，直接读像素最准：'#' 是边框色，'.' 是透明/背景，'W' 是窗口
# 自己的内容。弧画对了应该看到一道斜着的 '#'，画成直角就是一个实心的方角。
#
#   python tools\dump-corner.py         左上角
#   python tools\dump-corner.py 28      看 28x28 的范围
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


def dwmr(h):
    r = wintypes.RECT()
    if d.DwmGetWindowAttribute(wintypes.HWND(h), 9, ctypes.byref(r), ctypes.sizeof(r)) != 0:
        return None
    return (r.left, r.top, r.right, r.bottom)


def rc(h):
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


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
        off = (iy * w + ix) * 4
        return (buf[off + 2][0], buf[off + 1][0], buf[off][0])

    return px


ACTIVE = (0x62, 0x74, 0xE7)
INACTIVE = (0x70, 0x80, 0xAA)


def near(p, ref, tol=40):
    return all(abs(p[i] - ref[i]) <= tol for i in range(3))


span = int(sys.argv[1]) if len(sys.argv) > 1 else 20

fg = u.GetForegroundWindow()
f = dwmr(fg) or rc(fg)
print('前台 %s (%s)' % (pname(fg), cls(fg)))
print('DWM 边界 %s' % str(f))
print()

# 以窗口左上角为原点，往外取 span/2、往内取 span/2
margin = span // 2
x0 = f[0] - margin
y0 = f[1] - margin
px = grab(x0, y0, span, span)

print('左上角 %dx%d 像素，窗口边界在第 %d 行/列（0 起算）：' % (span, span, margin))
print('  # = 边框色   W = 窗口内容   . = 背景')
print()
print('     ' + ''.join(str(i % 10) for i in range(span)))
for iy in range(span):
    row = []
    for ix in range(span):
        c = px(ix, iy)
        if near(c, ACTIVE) or near(c, INACTIVE):
            row.append('#')
        elif ix >= margin and iy >= margin:
            row.append('W')
        else:
            row.append('.')
    mark = ' <- 窗口上边界' if iy == margin else ''
    print('  %2d %s%s' % (iy, ''.join(row), mark))
print()
print('     ' + ' ' * margin + '^ 窗口左边界')
