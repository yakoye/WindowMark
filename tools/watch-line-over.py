# 盯着一个窗口，抓「边框线画到了排在它上面的窗口身上」这一刻。
#
# 光看到「有线穿过对话框」不够——那条线可能来自一个确实排在对话框上面的窗口，那是
# 正常的层叠，不是 bug。判据得问系统：线经过的那些点上，最上面的窗口是谁？如果系统
# 说是对话框自己，边框却盖在那儿，才是真的画错了。
#
# 这类问题只在某个瞬间成立，等人复现完再手动跑工具，挪一下窗口就重画了，现象自己就
# 没了。所以让工具守着，命中那一帧连同 z 序、样式、命中测试结果一起冻下来。
#
#   py tools\watch-line-over.py mobaxterm TFormParams
import ctypes
import io
import os
import sys
import time
from ctypes import wintypes

# 程序内部的诊断日志。命中那一刻把它一起读出来，才能把「屏幕上看到的」和「程序当时
# 认为的」对上。
DIAG_DIR = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'WindowMark')
DIAG_FLAG = os.path.join(DIAG_DIR, 'diag.on')
DIAG_LOG = os.path.join(DIAG_DIR, 'diag.log')


def diag_on():
    try:
        if os.path.isfile(DIAG_LOG):
            os.remove(DIAG_LOG)
        with io.open(DIAG_FLAG, 'w') as fh:
            fh.write('on')
        return True
    except OSError:
        return False


def diag_off():
    try:
        os.remove(DIAG_FLAG)
    except OSError:
        pass


def diag_tail(keep=6):
    if not os.path.isfile(DIAG_LOG):
        return []
    try:
        with io.open(DIAG_LOG, encoding='utf-8-sig') as fh:
            lines = [l.rstrip() for l in fh if '快照' in l]
        return lines[-keep:]
    except OSError:
        return []

u = ctypes.WinDLL('user32', use_last_error=True)
g = ctypes.WinDLL('gdi32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND
u.GetTopWindow.restype = wintypes.HWND
u.GetWindow.restype = wintypes.HWND
u.GetAncestor.restype = wintypes.HWND
u.WindowFromPoint.restype = wintypes.HWND
u.WindowFromPoint.argtypes = [wintypes.POINT]
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

SRCCOPY = 0x00CC0020
ACTIVE = (0x62, 0x74, 0xE7)
INACTIVE = (0x70, 0x80, 0xAA)
GW_HWNDNEXT = 2
GW_OWNER = 4
GA_ROOT = 2
GWL_EXSTYLE = -20
GWL_STYLE = -16
WS_EX_TOPMOST = 0x00000008
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
WS_POPUP = 0x80000000
WS_CHILD = 0x40000000
P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
MARGIN = 8
MIN_RUN = 40


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


def title(h):
    b = ctypes.create_unicode_buffer(200)
    u.GetWindowTextW(h, b, 200)
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


def top_at(x, y):
    """系统认为这个点上最上面的顶级窗口是谁。"""
    h = u.WindowFromPoint(wintypes.POINT(x, y))
    return u.GetAncestor(h, GA_ROOT) if h else None


# 关键字可以给好几个，进程名和类名都算匹配。一个都不给就盯所有够大的可见窗口。
wants = [a.lower() for a in sys.argv[1:] if not a.startswith('-')]


def locate_all():
    out = []

    def cb(h, _):
        if not u.IsWindowVisible(h) or u.IsIconic(h):
            return True
        if cls(h).startswith('WindowMark.'):
            return True
        if cloaked(h):
            return True
        if wants:
            low_p = pname(h).lower()
            low_c = cls(h).lower()
            if not any(w in low_p or w in low_c for w in wants):
                return True
        r = frame(h)
        # 太小的窗口画不出「被线切开」的效果，也扫得慢，跳过。
        if r[2] - r[0] > 200 and r[3] - r[1] > 150:
            out.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    return out


scr = u.GetDC(0)


def scan(f):
    x0, y0 = f[0] + MARGIN, f[1] + MARGIN
    w, h = f[2] - f[0] - MARGIN * 2, f[3] - f[1] - MARGIN * 2
    if w <= 0 or h <= 0:
        return [], []
    md = g.CreateCompatibleDC(scr)
    bm = g.CreateCompatibleBitmap(scr, w, h)
    g.SelectObject(md, bm)
    g.BitBlt(md, 0, 0, w, h, scr, x0, y0, SRCCOPY)
    info = BITMAPINFO()
    info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.bmiHeader.biWidth = w
    info.bmiHeader.biHeight = -h
    info.bmiHeader.biPlanes = 1
    info.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(md, bm, 0, h, buf, ctypes.byref(info), 0)
    g.DeleteObject(bm)
    g.DeleteDC(md)

    def hit(ix, iy):
        off = (iy * w + ix) * 4
        p = (buf[off + 2][0], buf[off + 1][0], buf[off][0])
        return any(all(abs(p[i] - ref[i]) <= 12 for i in range(3))
                   for ref in (ACTIVE, INACTIVE))

    verts = []
    for ix in range(0, w, 2):
        run = best = 0
        for iy in range(h):
            run = run + 1 if hit(ix, iy) else 0
            best = max(best, run)
        if best >= MIN_RUN:
            verts.append(x0 + ix)
    hors = []
    for iy in range(0, h, 2):
        run = best = 0
        for ix in range(w):
            run = run + 1 if hit(ix, iy) else 0
            best = max(best, run)
        if best >= MIN_RUN:
            hors.append(y0 + iy)
    return verts, hors


def judge(target, verts, hors):
    """线经过的地方，系统说最上面的是不是目标自己？是的话就是画错了。"""
    f = frame(target)
    bad = []
    for x in verts:
        for t in (0.3, 0.5, 0.7):
            y = f[1] + int((f[3] - f[1]) * t)
            owner = top_at(x, y)
            if owner and int(owner) == int(target):
                bad.append(('竖线', x, y))
                break
    for y in hors:
        for t in (0.3, 0.5, 0.7):
            x = f[0] + int((f[2] - f[0]) * t)
            owner = top_at(x, y)
            if owner and int(owner) == int(target):
                bad.append(('横线', y, x))
                break
    return bad


def dump_scene(target, verts, hors, bad):
    fg = u.GetForegroundWindow()
    print()
    print('=== 抓到了：边框画在了排在它上面的窗口身上 ===')
    print('目标 %s (%s) %s' % (pname(target), cls(target), str(frame(target))))
    print('     标题：%s' % (title(target) or '(无)'))
    print('前台 %s (%s)' % (pname(fg), cls(fg)))
    print()
    print('画错的位置（系统的命中测试说这些点上最上面就是目标自己）：')
    for kind, a, b in bad:
        if kind == '竖线':
            print('  竖线 x=%d（在 y=%d 处验证）' % (a, b))
        else:
            print('  横线 y=%d（在 x=%d 处验证）' % (a, b))
    print()
    tf = frame(target)
    # 线对上了谁的窗口边缘，就是谁的边框跑过来了
    print('这些线对应谁的窗口边缘：')
    order = []
    hwnd = u.GetTopWindow(None)
    guard = 0
    while hwnd and guard < 4096:
        guard += 1
        if u.IsWindowVisible(hwnd) and not cls(hwnd).startswith('WindowMark.') \
                and not cloaked(hwnd):
            r = frame(hwnd)
            if r[2] > r[0] and r[3] > r[1]:
                order.append(int(hwnd))
        hwnd = u.GetWindow(hwnd, GW_HWNDNEXT)
    for kind, a, _ in bad:
        for h in order:
            r = frame(h)
            cands = ((r[0], '左'), (r[2], '右')) if kind == '竖线' else ((r[1], '上'), (r[3], '下'))
            for edge, side in cands:
                if abs(a - edge) <= 4:
                    ex = u.GetWindowLongPtrW(h, GWL_EXSTYLE)
                    owner = u.GetWindow(h, GW_OWNER)
                    print('  %s %d  ←  %s (%s) 的%s边，z=%d%s%s'
                          % (kind, a, pname(h), cls(h)[:24], side, order.index(h),
                             '，TOPMOST' if (ex & WS_EX_TOPMOST) else '',
                             '，是前台' if int(h) == int(fg) else ''))
                    if owner:
                        print('        它的 owner 是 %s (%s)'
                              % (pname(owner), cls(owner)[:24]))
                    print('        目标在 z 序里排第 %d——%s'
                          % (order.index(int(target)),
                             '比它靠前，所以边框不该盖过来'
                             if order.index(int(target)) < order.index(h)
                             else '比它靠后，层叠上说得通，问题在别处'))
    print()
    print('程序内部那一帧的快照（它自己认为的 z 序和前台）：')
    tail = diag_tail()
    if tail:
        for line in tail:
            print('  ' + line)
    else:
        print('  （没读到——诊断没开，或者这段时间边框没重画）')
    print()
    print('z 序（从上到下，只列和目标重叠的）：')
    for i, h in enumerate(order[:30]):
        r = frame(h)
        if r[2] <= tf[0] or r[0] >= tf[2] or r[3] <= tf[1] or r[1] >= tf[3]:
            continue
        ex = u.GetWindowLongPtrW(h, GWL_EXSTYLE)
        st = u.GetWindowLongPtrW(h, GWL_STYLE)
        owner = u.GetWindow(h, GW_OWNER)
        flags = []
        if ex & WS_EX_TOPMOST:
            flags.append('TOPMOST')
        if ex & WS_EX_TOOLWINDOW:
            flags.append('TOOL')
        if ex & WS_EX_NOACTIVATE:
            flags.append('NOACT')
        if st & WS_POPUP:
            flags.append('POPUP')
        if st & WS_CHILD:
            flags.append('CHILD')
        mark = '  <<< 目标' if h == int(target) else ('  <<< 前台' if h == int(fg) else '')
        print('  %2d %-20s %-26s %-26s %s%s'
              % (i, pname(h)[:20], cls(h)[:26], str(r), ' '.join(flags), mark))
        if owner:
            print('       owner=%s(%s)' % (pname(owner), cls(owner)[:20]))


started_diag = diag_on()
print('盯着：%s，最多守 150 秒。%s'
      % (' / '.join(wants) if wants else '所有够大的可见窗口',
         '（已顺手打开程序诊断）' if started_diag else '（打不开程序诊断，只报屏幕这边）'))
print('只在真的画错时才停：线穿过的地方，系统说最上面就是这个窗口本身。')
print('请你复现——开关对话框、在几个窗口之间来回点。')
sys.stdout.flush()

deadline = time.time() + 150
normal = 0
seen = set()
caught = False
while time.time() < deadline and not caught:
    targets = locate_all()
    for name in sorted({'%s/%s' % (pname(h), cls(h)) for h in targets}):
        if name not in seen:
            seen.add(name)
            print('  盯上了 %s' % name)
            sys.stdout.flush()
    for target in targets:
        f = frame(target)
        verts, hors = scan(f)
        if not verts and not hors:
            continue
        if frame(target) != f:
            continue                  # 采样期间窗口动了
        bad = judge(target, verts, hors)
        if bad:
            dump_scene(target, verts, hors, bad)
            caught = True
            break
        normal += 1
    time.sleep(0.1)

if not caught:
    print()
    print('守到时间了，没抓到画错的情况。')
    print('期间有 %d 次「窗口内部有边框线」，但每次系统都说那里最上面是别的窗口，'
          '也就是正常的层叠遮挡。' % normal)
if started_diag:
    diag_off()
u.ReleaseDC(0, scr)
