# 列出每个大窗口的 owned 窗口——尤其是那些「可见但极小」的辅助窗口。
#
# 边框的规则里有一条：目标有自己的对话框浮在上面时，边框要排到对话框之下。判断
# 「是不是对话框」如果只看 IsWindowVisible，Electron/Chrome 那一堆 1x1 的辅助窗口
# 就会把边框拖进普通层，于是被别的普通窗口盖住——表现为边框线不全。
import ctypes
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)

P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
GW_OWNER, GW_HWNDPREV = 4, 3
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass


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


def enum(pred):
    out = []

    def cb(h, _):
        if pred(h):
            out.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    return out


def overlaps(a, b):
    return not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])


# 复刻 LowestOwnedDialog 的走法：从目标往上走它自己的 owned 窗口
def lowest_owned(target):
    probe = target
    for _ in range(4096):
        probe = u.GetWindow(probe, GW_HWNDPREV)
        if not probe:
            break
        if not u.IsWindowVisible(probe):
            continue
        if u.GetWindow(probe, GW_OWNER) != target:
            break
        # 和 C++ 侧一致：只认有实际尺寸、且和目标有重叠的。0x0 的辅助窗口
        # （PseudoConsoleWindow 之流）不算对话框。
        pr = rc(probe)
        if (pr[2] - pr[0]) <= 0 or (pr[3] - pr[1]) <= 0:
            continue
        if not overlaps(pr, rc(target)):
            continue
        return probe
    return None


fg = u.GetForegroundWindow()
print('前台：%s (%s)' % (pname(fg), cls(fg)))
print()

targets = []
for h in enum(lambda x: u.IsWindowVisible(x) and not u.IsIconic(x)):
    if cls(h).startswith('WindowMark.'):
        continue
    r = rc(h)
    if (r[2] - r[0]) < 300 or (r[3] - r[1]) < 200:
        continue
    targets.append(h)

for h in targets:
    hr = rc(h)
    dlg = lowest_owned(h)
    owned = [o for o in enum(lambda x: u.IsWindowVisible(x))
             if u.GetWindow(o, GW_OWNER) == h]
    if not owned and dlg is None:
        continue
    mark = ' <<< 前台' if h == fg else ''
    print('%s (%s) %s%s' % (pname(h), cls(h)[:26], str(hr), mark))
    for o in owned:
        orc = rc(o)
        w, ht = orc[2] - orc[0], orc[3] - orc[1]
        flags = []
        if w * ht == 0:
            flags.append('零尺寸')
        elif w < 100 or ht < 50:
            flags.append('极小')
        if not overlaps(orc, hr):
            flags.append('不重叠')
        if o == dlg:
            flags.append('** 边框会排到它之下 **')
        print('    owned %-24s %-26s %dx%-5d %s'
              % (cls(o)[:24], str(orc), w, ht, ' '.join(flags)))
    if dlg is not None and dlg not in owned:
        print('    锚点 %s %s（不在 owned 列表里？）' % (cls(dlg), str(rc(dlg))))
    print()
