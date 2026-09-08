# 把某个进程的所有可见窗口连同 z 序、归属关系一起打出来，并按边框的裁剪规则算一遍
# 「谁的边框会画到谁身上」。
#
# 边框穿过一个对话框，原因只可能是三条规则之一没对上：那个对话框不是 topmost、不是
# 被穿过那个窗口的 owned 窗口、而被穿过的那个窗口又恰好是前台（前台只被这两类东西
# 裁）。三条各自对应不同的修法，光看截图分不出是哪一条。
#
#   py tools\diagnose-window.py mobaxterm
#   py tools\diagnose-window.py            前台窗口所属的进程
import ctypes
import sys
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND
u.GetTopWindow.restype = wintypes.HWND
u.GetWindow.restype = wintypes.HWND
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

GW_HWNDNEXT = 2
GW_OWNER = 4
GWL_EXSTYLE = -20
GWL_STYLE = -16
WS_EX_TOPMOST = 0x00000008
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
WS_POPUP = 0x80000000
WS_CHILD = 0x40000000


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


def overlap(a, b):
    return not (a[2] <= b[0] or a[0] >= b[2] or a[3] <= b[1] or a[1] >= b[3])


# 按 CaptureDesktop 的口径走一遍 z 序：从上到下，跳过隐藏的、cloaked 的、自家的。
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

fg = u.GetForegroundWindow()
keyword = sys.argv[1].lower() if len(sys.argv) > 1 else pname(fg).lower()

print('前台：%s (%s) %s' % (pname(fg), cls(fg), str(frame(fg))))
print('关键字：%s' % keyword)
print()

mine = [h for h in order if keyword in pname(h).lower() or keyword in cls(h).lower()]
if not mine:
    print('没找到匹配的可见窗口。')
    raise SystemExit(1)

print('这个进程的可见窗口（按 z 序从上到下）：')
print()
for h in mine:
    ex = u.GetWindowLongPtrW(h, GWL_EXSTYLE)
    st = u.GetWindowLongPtrW(h, GWL_STYLE)
    owner = u.GetWindow(h, GW_OWNER)
    print('  z=%-4d %-28s %s' % (order.index(h), cls(h)[:28], str(frame(h))))
    print('         标题：%s' % (title(h) or '(无)'))
    flags = []
    if ex & WS_EX_TOPMOST:
        flags.append('TOPMOST')
    if ex & WS_EX_TOOLWINDOW:
        flags.append('TOOLWINDOW')
    if ex & WS_EX_NOACTIVATE:
        flags.append('NOACTIVATE')
    if st & WS_POPUP:
        flags.append('POPUP')
    if st & WS_CHILD:
        flags.append('CHILD')
    print('         样式：%s' % (' '.join(flags) or '（都没有）'))
    if owner:
        print('         owner：%s (%s) z=%s'
              % (pname(owner), cls(owner)[:24],
                 order.index(int(owner)) if int(owner) in order else '不在链上'))
    else:
        print('         owner：无')
    print('         是前台：%s' % ('是' if int(h) == int(fg) else '否'))
    print()

# 按 PlanBorders 的规则算：谁的边框会盖到这些窗口上
print('按边框的裁剪规则推演——谁的边框会画到这些窗口身上：')
print()
targets = {h: frame(h) for h in mine}
found = False
for i, drawer in enumerate(order):
    drawer_rect = frame(drawer)
    is_fg = int(drawer) == int(fg)
    for h in mine:
        if h == drawer:
            continue
        # 边框环大致就是窗口矩形往外两三像素，重叠判断用矩形本身足够
        if not overlap(drawer_rect, targets[h]):
            continue
        above = order.index(h) < i          # h 排在 drawer 上面
        if not above:
            continue
        ex = u.GetWindowLongPtrW(h, GWL_EXSTYLE)
        owner = u.GetWindow(h, GW_OWNER)
        h_topmost = bool(ex & WS_EX_TOPMOST)
        h_owned_by_drawer = bool(owner) and int(owner) == int(drawer)
        if is_fg:
            clipped = h_topmost or h_owned_by_drawer
            why = ('会被裁掉（%s）' % ('对方是 TOPMOST' if h_topmost else '对方是它的 owned 窗口')
                   ) if clipped else '**不会被裁——边框会画在它上面**'
        else:
            clipped = True
            why = '会被裁掉（画边框的这个不是前台，排在它上面的一律算遮挡）'
        print('  %s (%s)%s 的边框 vs %s (%s)：%s'
              % (pname(drawer), cls(drawer)[:20], '［前台］' if is_fg else '',
                 pname(h), cls(h)[:20], why))
        if not clipped:
            found = True

if not found:
    print('  （没有算出会穿过去的情况——现象可能出在别处，或者刚才那一瞬状态不同）')
