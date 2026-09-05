# 专查「孤立浮线」：边框停在 topmost 层，而它的目标并不是前台窗口。
#
# 这就是屏幕上那条与任何窗口都不搭界的蓝线的准确定义——边框浮在盖住目标的那些
# 窗口上面，画出目标的轮廓，而目标本身在那儿是看不见的。
#
#   python tools\check-floating-borders.py          单次检查
#   python tools\check-floating-borders.py 20       连续查 20 秒（切换窗口时跑这个）
import ctypes
import sys
import time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
WS_EX_TOPMOST = 0x8
GW_HWNDNEXT = 2
BORDER_CLASS = 'WindowMark.WindowBorder'


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


def zlist():
    out = []
    h = u.GetTopWindow(None)
    while h:
        out.append(h)
        h = u.GetWindow(h, GW_HWNDNEXT)
    return out


def targets():
    """普通的顶级窗口，用来给边框找对象。"""
    return enum(lambda h: u.IsWindowVisible(h) and not u.IsIconic(h)
                and not cls(h).startswith('WindowMark.')
                and (rc(h)[2] - rc(h)[0]) > 120 and (rc(h)[3] - rc(h)[1]) > 80)


def pair(border, cands):
    """按中心距离给边框找它描的那个窗口。边框比窗口大 reach 像素，中心重合。"""
    br = rc(border)
    bcx, bcy = (br[0] + br[2]) / 2, (br[1] + br[3]) / 2
    best, bestd = None, 10 ** 9
    for h in cands:
        f = dwm(h) or rc(h)
        dist = abs((f[0] + f[2]) / 2 - bcx) + abs((f[1] + f[3]) / 2 - bcy)
        if dist < bestd:
            bestd, best = dist, h
    return (best, bestd) if bestd <= 40 else (None, bestd)


def check(verbose=True):
    fg = u.GetForegroundWindow()
    order = zlist()
    idx = {h: i for i, h in enumerate(order)}
    cands = targets()
    borders = enum(lambda h: u.IsWindowVisible(h) and cls(h) == BORDER_CLASS)

    floating = []
    for b in borders:
        if (u.GetWindowLongPtrW(b, -20) & WS_EX_TOPMOST) == 0:
            continue
        owner, dist = pair(b, cands)
        if owner is None:
            floating.append((b, None, '找不到对应窗口(最近 %d px)' % dist))
        elif owner != fg:
            ownerTopmost = (u.GetWindowLongPtrW(owner, -20) & WS_EX_TOPMOST) != 0
            if not ownerTopmost:
                floating.append((b, owner, '目标不是前台'))

    if verbose:
        print('前台：%s (%s)' % (pname(fg), cls(fg)))
        print('边框共 %d 个，其中 topmost 的 %d 个'
              % (len(borders),
                 sum(1 for b in borders
                     if u.GetWindowLongPtrW(b, -20) & WS_EX_TOPMOST)))
    for b, owner, why in floating:
        who = ('%s(%s)' % (pname(owner), cls(owner)[:20])) if owner else '?'
        print('  !! 孤立浮线  边框 z=%-4d 矩形%-28s 目标 %s  —— %s'
              % (idx.get(b, -1), str(rc(b)), who, why))
    if verbose and not floating:
        print('  没有孤立浮线')
    return floating


seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 0
if seconds <= 0:
    check()
else:
    print('连续检查 %d 秒——请在这段时间里来回切换窗口（含任务管理器）' % seconds)
    print()
    end = time.time() + seconds
    hits = 0
    samples = 0
    while time.time() < end:
        samples += 1
        bad = check(verbose=False)
        if bad:
            hits += 1
        time.sleep(0.2)
    print()
    print('采样 %d 次，其中 %d 次发现孤立浮线。' % (samples, hits))
    if hits == 0:
        print('没有边框浮到别的窗口上面。')
