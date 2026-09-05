# 查「应用自己的对话框被边框线穿过」：Word 的查找替换、Notepad++ 的查找都是这种
# owned window——它们有 owner，在 z 序上永远紧贴在 owner 上面。
#
# 对话框打开时前台是对话框，不是主窗口，所以主窗口的边框应该降到主窗口下方，被
# 对话框盖住。这个脚本把真实状态打出来，看是哪一步没成。
#
#   python tools\check-dialog-borders.py       打开那个对话框，然后跑这个
import ctypes
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
GW_OWNER = 4
GW_HWNDNEXT = 2
WS_EX_TOPMOST = 0x8
WS_EX_TOOLWINDOW = 0x80
BORDER_CLASS = 'WindowMark.WindowBorder'


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


def topmost(h):
    return (u.GetWindowLongPtrW(h, -20) & WS_EX_TOPMOST) != 0


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


def find_border(target, borders):
    f = dwm(target) or rc(target)
    cx, cy = (f[0] + f[2]) / 2, (f[1] + f[3]) / 2
    best, bestd = None, 10 ** 9
    for b in borders:
        r = rc(b)
        dist = abs((r[0] + r[2]) / 2 - cx) + abs((r[1] + r[3]) / 2 - cy)
        if dist < bestd:
            bestd, best = dist, b
    return best if bestd <= 60 else None


def overlaps(a, b):
    return not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])


order = zlist()
idx = {h: i for i, h in enumerate(order)}
fg = u.GetForegroundWindow()
borders = enum(lambda h: u.IsWindowVisible(h) and cls(h) == BORDER_CLASS)

# 有 owner、owner 是个真窗口的可见顶级窗口 = 应用自己的对话框
dialogs = []
for h in enum(lambda x: u.IsWindowVisible(x) and not u.IsIconic(x)):
    if cls(h).startswith('WindowMark.'):
        continue
    owner = u.GetWindow(h, GW_OWNER)
    if not owner or not u.IsWindowVisible(owner):
        continue
    oex = u.GetWindowLongPtrW(owner, -20)
    orc = rc(owner)
    if (oex & WS_EX_TOOLWINDOW) or (orc[2] - orc[0]) <= 0 or (orc[3] - orc[1]) <= 0:
        continue
    r = rc(h)
    if (r[2] - r[0]) < 150 or (r[3] - r[1]) < 100:
        continue
    dialogs.append((h, owner))

print('前台：%s (%s) "%s"' % (pname(fg), cls(fg), title(fg)[:40]))
print('找到 %d 个应用自己的对话框' % len(dialogs))
print()

if not dialogs:
    print('没有打开的对话框。先把 Word 的查找替换或 Notepad++ 的查找打开，再跑这个。')
    raise SystemExit(0)

for dlg, owner in dialogs:
    b = find_border(owner, borders)
    print('对话框  %s "%s"' % (cls(dlg), title(dlg)[:36]))
    print('   z=%-4d 矩形%-28s %s%s'
          % (idx.get(dlg, -1), str(rc(dlg)),
             'topmost ' if topmost(dlg) else '',
             '前台' if dlg == fg else ''))
    print('   owner  %s (%s)  z=%-4d %s'
          % (pname(owner), cls(owner), idx.get(owner, -1),
             'topmost' if topmost(owner) else ''))
    if b is None:
        print('   owner 没有配对的边框窗口')
        print()
        continue

    zb, zd, zo = idx.get(b, -1), idx.get(dlg, -1), idx.get(owner, -1)
    br = rc(b)
    print('   边框   z=%-4d 矩形%-28s %s'
          % (zb, str(br), 'topmost <<<' if topmost(b) else ''))

    if not overlaps(br, rc(dlg)):
        print('   边框和对话框不重叠，看不到穿透')
    elif zb < zd:
        print('   !! 边框排在对话框**前面** %d 位 —— 线会穿过对话框' % (zd - zb))
        if topmost(b) and not topmost(dlg):
            print('      原因：边框还留在 topmost 层，而对话框不是 topmost')
        else:
            print('      原因：同层但边框更靠前')
    else:
        print('   边框排在对话框后面 %d 位，对话框会盖住它（正确）' % (zb - zd))
    print('   z 序：对话框 %d < owner %d < 边框 %d ?  %s'
          % (zd, zo, zb, '是' if zd < zo < zb else '否'))
    print()
