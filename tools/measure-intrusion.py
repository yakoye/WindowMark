# 边框往窗口**里面**吃了几像素。
#
# 和 measure-stroke.py 不同：那个量的是线有多粗，这个量的是「窗口自己的内容被盖掉多少」。
# 线宽一样但整条线往里挪，measure-stroke 看不出区别，用户却一眼就看出来了。
#
# 参照系是 DWM 的扩展边界（DWMWA_EXTENDED_FRAME_BOUNDS，即 WinBorderPlan 里的 frame），
# 不是 GetWindowRect——后者在 Win10+ 包含一圈透明的投影余量，拿它当窗口边缘会把结论整体
# 偏掉好几像素。
#
# 直边和圆角分开报：它们走的是两套宽度（stroke 对 stroke+corner_width_extra）和两套
# 定位（reach 对 roundWidth/2 + corner_inset），完全可能一个正常另一个偏。
# 圆角沿**半径方向**量，沿水平/垂直量弧量到的是斜边。
#
#   py tools\measure-intrusion.py
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
    buf = ctypes.create_string_buffer(sw * sh * 4)
    g.GetDIBits(md, bm, 0, sh, buf, ctypes.byref(info), 0)
    g.DeleteObject(bm)
    g.DeleteDC(md)
    u.ReleaseDC(0, scr)
    return buf.raw


PIX = grab_screen()


def at(x, y):
    px, py = x - ox, y - oy
    if px < 0 or py < 0 or px >= sw or py >= sh:
        return None
    i = (py * sw + px) * 4
    return (PIX[i + 2], PIX[i + 1], PIX[i])          # BGRA -> RGB


def near(c, ref, tol):
    return c is not None and all(abs(c[i] - ref[i]) <= tol for i in range(3))


def is_border(c, tol):
    return near(c, ACTIVE, tol) or near(c, INACTIVE, tol)


hwnd = u.GetForegroundWindow()
frame = dwmr(hwnd)
window = rc(hwnd)
if frame is None:
    print('DWM 不给扩展边界，退回 GetWindowRect')
    frame = window

print('窗口：%s (%s)' % (pname(hwnd), cls(hwnd)))
print('GetWindowRect %s' % str(window))
print('DWM 扩展边界   %s   <- 这个才是边框对齐的基准' % str(frame))
print()

# 两档容差：严的只认实心，松的把抗锯齿的过渡也算进去。圆角是 D2D 带抗锯齿画的，
# 直边是像素填充没有抗锯齿，只用一档会把两者的差异算到错误的地方。
TOLS = ((28, '实心'), (90, '含抗锯齿'))


def walk(x0, y0, dx, dy, tol, limit=24):
    """从 (x0,y0) 沿 (dx,dy) 一步步走，数连续多少个像素是边框色。"""
    n = 0
    while n < limit:
        c = at(x0 + dx * n, y0 + dy * n)
        if not is_border(c, tol):
            break
        n += 1
    return n


L, T, R, B = frame
midX, midY = (L + R) // 2, (T + B) // 2

for tol, label in TOLS:
    print('--- %s（容差 %d）---' % (label, tol))

    # 直边：从窗口边缘往**里**走，数被盖掉几像素；再从边缘往外走，数伸出去几像素。
    edges = (
        ('左边', L, midY, 1, 0, -1, 0),
        ('右边', R - 1, midY, -1, 0, 1, 0),
        ('上边', midX, T, 0, 1, 0, -1),
        ('下边', midX, B - 1, 0, -1, 0, 1),
    )
    for name, x, y, ix, iy, oxd, oyd in edges:
        inside = walk(x, y, ix, iy, tol)
        outside = walk(x + oxd, y + oyd, oxd, oyd, tol)
        print('  %s  窗口内 %2d px   窗口外 %2d px   共 %2d px'
              % (name, inside, outside, inside + outside))

    # 圆角：沿半径方向量。圆心在距两边各 r 的位置，r 取 Windows 11 的圆角半径（DIP 8
    # 乘以缩放），量的方向是圆心指向角外的 45 度。
    dpi = u.GetDpiForWindow(hwnd) or 96
    r = 8.0 * dpi / 96.0
    diag = math.sqrt(0.5)
    corners = (
        ('左上', L + r, T + r, -diag, -diag),
        ('右上', R - r, T + r, diag, -diag),
        ('左下', L + r, B - r, -diag, diag),
        ('右下', R - r, B - r, diag, diag),
    )
    for name, cx, cy, ux, uy in corners:
        # 从圆心沿半径往外一步步走：先找到边框色开始的地方，再找结束的地方。
        first = last = None
        step = 0.0
        while step < 40.0:
            c = at(int(round(cx + ux * step)), int(round(cy + uy * step)))
            if is_border(c, tol):
                if first is None:
                    first = step
                last = step
            elif first is not None and step - last > 2.0:
                break
            step += 0.5
        if first is None:
            print('  %s角  没找到边框色' % name)
            continue
        # 弧的内沿 / 外沿到窗口自身弧（半径 r）的距离，正数表示在窗口里面
        print('  %s角  内沿吃进窗口 %4.1f px   外沿伸出 %4.1f px   弧宽 %4.1f px'
              % (name, r - first, last - r, last - first + 1))
    print()

print('对照配置：border.width=stroke，reach=stroke+border.offset（= 伸出多少）,')
print('窗口内应盖 stroke-reach = -offset 像素；圆角另走 stroke+corner_width_extra，')
print('路径内收 roundWidth/2 + corner_inset。')
