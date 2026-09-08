# 量边框上的缺口，并把它和「谁挡的」对上。
#
# 边框被别的窗口挡住时会断开一截，问题是这一截该有多长。正确的答案是「正好等于那个
# 窗口盖住的范围」，多一分就显得是断的而不是被挡的。
#
# 缺口的两端坐标和遮挡窗口的矩形一比就知道差在哪：差值为零说明裁得刚好，差值为正说明
# 多裁了——那要么是裁剪算法的问题，要么是那个窗口的矩形本来就比它看得见的部分大
# （带阴影、圆角、或者干脆留了一圈透明边的弹出面板都这样）。
#
#   py tools\measure-gap.py              前台窗口
#   py tools\measure-gap.py explorer
#   py tools\measure-gap.py --all    扫所有窗口，只报多裁的地方
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


def dwm_frame(h):
    r = wintypes.RECT()
    if d.DwmGetWindowAttribute(wintypes.HWND(h), 9, ctypes.byref(r), ctypes.sizeof(r)) != 0:
        u.GetWindowRect(wintypes.HWND(h), ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def win_rect(h):
    r = wintypes.RECT()
    u.GetWindowRect(wintypes.HWND(h), ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def cloaked(h):
    v = ctypes.c_int(0)
    d.DwmGetWindowAttribute(wintypes.HWND(h), 14, ctypes.byref(v), ctypes.sizeof(v))
    return v.value != 0


def find(keyword):
    out = []

    def cb(h, _):
        if u.IsWindowVisible(h) and not u.IsIconic(h) and not cls(h).startswith('WindowMark.'):
            r = dwm_frame(h)
            if (r[2] - r[0]) > 300 and (r[3] - r[1]) > 200:
                if keyword in pname(h).lower() or keyword in cls(h).lower():
                    out.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    return out[0] if out else None


scan_all = '--all' in sys.argv
args = [a for a in sys.argv[1:] if not a.startswith('-')]

targets = []
if scan_all:
    def collect(h, _):
        if u.IsWindowVisible(h) and not u.IsIconic(h) and not cls(h).startswith('WindowMark.'):
            r = dwm_frame(h)
            if (r[2] - r[0]) > 300 and (r[3] - r[1]) > 200 and not cloaked(h):
                targets.append(h)
        return True

    u.EnumWindows(P(collect), 0)
else:
    one = find(args[0].lower()) if args else u.GetForegroundWindow()
    if not one:
        print('找不到窗口')
        raise SystemExit(1)
    targets = [one]

print('前台 %s' % pname(u.GetForegroundWindow()))
print()

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
raw = ctypes.create_string_buffer(sw * sh * 4)
g.GetDIBits(md, bm, 0, sh, raw, ctypes.byref(info), 0)
g.DeleteObject(bm)
g.DeleteDC(md)
u.ReleaseDC(0, scr)


def at(x, y):
    ix, iy = x - ox, y - oy
    if ix < 0 or iy < 0 or ix >= sw or iy >= sh:
        return None
    off = (iy * sw + ix) * 4
    return (raw[off + 2][0], raw[off + 1][0], raw[off][0])


def painted(x, y, span=10):
    """(x,y) 附近垂直于边的方向上有没有边框色。"""
    for s in range(-span, span + 1):
        p = at(x if span == 0 else x, y)
        if p is None:
            continue
    return False


# 桌面上所有可见窗口，用来认领缺口
all_windows = []
hwnd = u.GetTopWindow(None)
guard = 0
while hwnd and guard < 4096:
    guard += 1
    if u.IsWindowVisible(hwnd) and not cls(hwnd).startswith('WindowMark.') and not cloaked(hwnd):
        r = dwm_frame(hwnd)
        if r[2] > r[0] and r[3] > r[1]:
            all_windows.append((pname(hwnd), cls(hwnd), r, win_rect(hwnd), int(hwnd)))
    hwnd = u.GetWindow(hwnd, GW_HWNDNEXT)

SPAN = 10


def has_border_at(x, y, horizontal):
    """沿垂直于边的方向扫 ±SPAN，看有没有边框色。"""
    for s in range(-SPAN, SPAN + 1):
        p = at(x, y + s) if horizontal else at(x + s, y)
        if p is None:
            continue
        if any(all(abs(p[i] - ref[i]) <= 12 for i in range(3)) for ref in (ACTIVE, INACTIVE)):
            return True
    return False


fg = u.GetForegroundWindow()
GWL_EXSTYLE = -20
GW_OWNER = 4
WS_EX_TOPMOST = 0x8

# 每个窗口的额外属性，判「谁能裁前台」时要用
extra = {}
for _p, _c, _r, _w, h in all_windows:
    ex = u.GetWindowLongPtrW(wintypes.HWND(h), GWL_EXSTYLE)
    owner = u.GetWindow(wintypes.HWND(h), GW_OWNER)
    extra[h] = (bool(ex & WS_EX_TOPMOST), int(owner) if owner else 0)

def has_any_border(f):
    """四条边上采样，一个边框色都找不到就说明这个窗口压根没有边框。"""
    for t in range(1, 20):
        x = f[0] + (f[2] - f[0]) * t // 20
        y = f[1] + (f[3] - f[1]) * t // 20
        if has_border_at(x, f[1], True) or has_border_at(x, f[3], True):
            return True
        if has_border_at(f[0], y, False) or has_border_at(f[2], y, False):
            return True
    return False


problems = 0
skipped = 0
for target in targets:
    f = dwm_frame(target)
    # 桌面、任务栏这些本来就没有边框，扫它们只会把整条边报成「该有却没有」。
    #
    # 光靠「边上有没有边框色」筛不掉桌面：它铺满整个虚拟桌面，边界上恰好压着别人的
    # 边框。所以先按尺寸和类名排掉 shell 自己的那几个。
    shell_classes = ('Progman', 'WorkerW', 'Shell_TrayWnd', 'Shell_SecondaryTrayWnd',
                     'TopLevelWindowForOverflowXamlIsland')
    if cls(target) in shell_classes:
        skipped += 1
        continue
    if (f[2] - f[0]) >= sw and (f[3] - f[1]) >= sh:
        skipped += 1
        continue
    if not has_any_border(f):
        skipped += 1
        continue
    hs = [o[4] for o in all_windows]
    if int(target) not in hs:
        continue
    tindex = hs.index(int(target))
    is_fg = int(target) == int(fg)
    # 能盖住它的窗口。前台只认 topmost 和它自己的 owned 对话框——这是 PlanBorders 的
    # 规则，照抄过来，否则前台窗口会被误报一大片。
    above = []
    for i, one in enumerate(all_windows):
        if i >= tindex:
            break
        topmost, owner = extra[one[4]]
        if is_fg and not topmost and owner != int(target):
            continue
        above.append(one[2])

    def covered(x, y):
        return any(r[0] <= x < r[2] and r[1] <= y < r[3] for r in above)

    header_printed = False
    for side, horizontal, fixed, lo, hi in (
            ('上', True, f[1], f[0], f[2]),
            ('下', True, f[3], f[0], f[2]),
            ('左', False, f[0], f[1], f[3]),
            ('右', False, f[2], f[1], f[3])):
        # 逐点：该有边框（没被任何上方窗口盖住）却没有
        bad = []
        run_start = None
        for v in range(lo, hi):
            x, y = (v, fixed) if horizontal else (fixed, v)
            should = not covered(x, y)
            has = has_border_at(v, fixed, True) if horizontal else has_border_at(fixed, v, False)
            if should and not has:
                if run_start is None:
                    run_start = v
            else:
                if run_start is not None and v - run_start >= 10:
                    bad.append((run_start, v - 1))
                run_start = None
        if run_start is not None and hi - run_start >= 10:
            bad.append((run_start, hi - 1))

        for a, b in bad:
            if not header_printed:
                print('%s (%s) %s%s'
                      % (pname(target), cls(target), str(f), '  [前台]' if is_fg else ''))
                header_printed = True
            problems += 1
            print('  %s边 %d..%d（%d px）该有边框却没有——这一段没有任何窗口盖着'
                  % (side, a, b, b - a + 1))
            # 挨着它的是谁？多半是那个窗口的矩形比它看得见的部分大
            mid = (a + b) // 2
            near = []
            for proc, c, r, wr, h in all_windows:
                if h == int(target):
                    continue
                if horizontal:
                    if not (r[1] <= fixed <= r[3]):
                        continue
                    d0 = min(abs(r[0] - b), abs(r[2] - a))
                else:
                    if not (r[0] <= fixed <= r[2]):
                        continue
                    d0 = min(abs(r[1] - b), abs(r[3] - a))
                if d0 <= 60:
                    near.append((d0, proc, c, r, wr))
            near.sort()
            for d0, proc, c, r, wr in near[:2]:
                print('      紧挨着的是 %s (%s) %s，边缘差 %d px'
                      % (proc, c[:24], str(r), d0))
                if wr != r:
                    print('      它的 GetWindowRect %s，和 DWM 边界差 (%d,%d,%d,%d)'
                          % (str(wr), r[0] - wr[0], r[1] - wr[1], wr[2] - r[2], wr[3] - r[3]))
            if near:
                print('      -> 若这段紧挨着某个窗口，多半是它自绘了阴影或留了透明边，')
                print('         窗口矩形比看得见的部分大。用 measure_shadow_inset.bat 量它。')

if problems == 0:
    print('没发现「该有边框却没有」的地方——所有缺口都对得上真实的遮挡。')
print('（跳过了 %d 个本来就没有边框的窗口）' % skipped)
