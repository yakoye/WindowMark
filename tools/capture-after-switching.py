# 快速来回切窗口，停手之后抓一次现场。
#
# 「切换到最后，别的窗口边框没消失」这类问题的关键在**停下来那一刻**：状态已经稳定，
# 屏幕上却还是错的。这说明最后一帧要么算错了，要么根本没画。两者的区别只能靠对比
# 三份东西看出来：
#
#   1. 系统此刻的真相：GetForegroundWindow + z 序
#   2. 程序内部那一帧的快照（诊断日志里）
#   3. 屏幕上实际画出来的边框
#
# 一和二对不上 = 快照过期；二和三对不上 = 渲染没跟上；一二一致而三错 = 规划算错了。
#
#   py tools\capture-after-switching.py
#   py tools\capture-after-switching.py terminal notepad
import ctypes
import io
import os
import sys
import time
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
GW_OWNER = 4
GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x8
P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

DIAG_DIR = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'WindowMark')
DIAG_FLAG = os.path.join(DIAG_DIR, 'diag.on')
DIAG_LOG = os.path.join(DIAG_DIR, 'diag.log')


class BIH(ctypes.Structure):
    _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG),
                ('biHeight', wintypes.LONG), ('biPlanes', wintypes.WORD),
                ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD),
                ('biSizeImage', wintypes.DWORD), ('biXPelsPerMeter', wintypes.LONG),
                ('biYPelsPerMeter', wintypes.LONG), ('biClrUsed', wintypes.DWORD),
                ('biClrImportant', wintypes.DWORD)]


class BI(ctypes.Structure):
    _fields_ = [('bmiHeader', BIH), ('bmiColors', wintypes.DWORD * 3)]


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


def zorder():
    out = []
    h = u.GetTopWindow(None)
    guard = 0
    while h and guard < 4096:
        guard += 1
        if u.IsWindowVisible(h) and not cls(h).startswith('WindowMark.') and not cloaked(h):
            r = frame(h)
            if r[2] > r[0] and r[3] > r[1]:
                out.append(int(h))
        h = u.GetWindow(h, GW_HWNDNEXT)
    return out


ox = u.GetSystemMetrics(76)
oy = u.GetSystemMetrics(77)
sw = u.GetSystemMetrics(78) or u.GetSystemMetrics(0)
sh = u.GetSystemMetrics(79) or u.GetSystemMetrics(1)


def grab():
    scr = u.GetDC(0)
    md = g.CreateCompatibleDC(scr)
    bm = g.CreateCompatibleBitmap(scr, sw, sh)
    g.SelectObject(md, bm)
    g.BitBlt(md, 0, 0, sw, sh, scr, ox, oy, SRCCOPY)
    info = BI()
    info.bmiHeader.biSize = ctypes.sizeof(BIH)
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


raw = None


def at(x, y):
    ix, iy = x - ox, y - oy
    if ix < 0 or iy < 0 or ix >= sw or iy >= sh:
        return None
    off = (iy * sw + ix) * 4
    return (raw[off + 2], raw[off + 1], raw[off])


def colour_at(x, y, horizontal, span=10):
    """这条边附近是什么颜色的边框：活动、非活动，还是没有。"""
    got = set()
    for s in range(-span, span + 1):
        p = at(x, y + s) if horizontal else at(x + s, y)
        if p is None:
            continue
        if all(abs(p[i] - ACTIVE[i]) <= 12 for i in range(3)):
            got.add('活动')
        elif all(abs(p[i] - INACTIVE[i]) <= 12 for i in range(3)):
            got.add('非活动')
    if not got:
        return '无'
    return '+'.join(sorted(got))


def edge_report(f):
    """四条边各取三个位置，看画的是什么色。"""
    out = []
    for side, horizontal, gen in (
            ('上', True, lambda t: (f[0] + int((f[2] - f[0]) * t), f[1])),
            ('下', True, lambda t: (f[0] + int((f[2] - f[0]) * t), f[3])),
            ('左', False, lambda t: (f[0], f[1] + int((f[3] - f[1]) * t))),
            ('右', False, lambda t: (f[2], f[1] + int((f[3] - f[1]) * t)))):
        seen = [colour_at(*gen(t), horizontal) for t in (0.25, 0.5, 0.75)]
        out.append('%s %s' % (side, '/'.join(seen)))
    return '  '.join(out)


words = [a.lower() for a in sys.argv[1:]] or ['terminal', 'notepad']

try:
    if os.path.isfile(DIAG_LOG):
        os.remove(DIAG_LOG)
    with io.open(DIAG_FLAG, 'w') as fh:
        fh.write('on')
    diag = True
except OSError:
    diag = False

print('诊断%s。现在开始来回快速切换那两个窗口，切到你看见问题为止，然后**停手别动**。'
      % ('已开' if diag else '打不开'))
print('检测到连续 2.5 秒没有焦点变化就取样。最多等 90 秒。')
sys.stdout.flush()

last_fg = None
still_since = None
deadline = time.time() + 90
switched = 0
while time.time() < deadline:
    fg = u.GetForegroundWindow()
    if int(fg or 0) != int(last_fg or 0):
        last_fg = fg
        switched += 1
        still_since = time.time()
    elif still_since and time.time() - still_since >= 2.5 and switched >= 3:
        break
    time.sleep(0.1)

try:
    os.remove(DIAG_FLAG)
except OSError:
    pass

if switched < 3:
    print('没检测到几次切换，是不是没操作？')
    raise SystemExit(0)

# 取样：先记系统真相，再抓屏，再确认这期间前台没变
fg = u.GetForegroundWindow()
order = zorder()
raw = grab()
if int(u.GetForegroundWindow()) != int(fg):
    print('取样期间焦点又变了，再跑一次')
    raise SystemExit(1)

print()
print('=== 停手后的现场（切换了 %d 次）===' % switched)
print('系统此刻：前台是 %s (%s)' % (pname(fg), cls(fg)))
print()
print('z 序（从上到下）和每个窗口边框实际画的颜色：')
for i, h in enumerate(order[:12]):
    f = frame(h)
    ex = u.GetWindowLongPtrW(wintypes.HWND(h), GWL_EXSTYLE)
    mark = '  <<< 前台' if h == int(fg) else ''
    tag = ' TOPMOST' if (ex & WS_EX_TOPMOST) else ''
    print('  %2d %-22s %-28s %s%s%s' % (i, pname(h)[:22], cls(h)[:28], str(f), tag, mark))
    print('       边框：%s' % edge_report(f))

print()
print('程序内部最后几帧的快照：')
if os.path.isfile(DIAG_LOG):
    with io.open(DIAG_LOG, encoding='utf-8-sig') as fh:
        lines = [l.rstrip() for l in fh if '快照' in l or 'Redraw' in l]
    for line in lines[-6:]:
        print('  ' + line)
    if not lines:
        print('  （日志里没有——这段时间边框一次都没重画）')
else:
    print('  （日志没生成——这段时间边框一次都没重画）')
