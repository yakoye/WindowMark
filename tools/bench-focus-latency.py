# 切换窗口时，边框出现/消失得有多快。
#
# 被动观察，不抢焦点：挂一个 EVENT_SYSTEM_FOREGROUND 钩子，使用者正常点窗口，
# 工具记录两件事的真实间隔——
#   出现：新前台窗口成为前台  ->  它的边框进入 topmost 层
#   消失：旧前台窗口失去前台  ->  它的边框退出 topmost 层
#
#   python tools\bench-focus-latency.py         观察 30 秒
#   python tools\bench-focus-latency.py 60      观察 60 秒
import ctypes
import sys
import threading
import time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

WINEVENTPROC = ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.DWORD, wintypes.HWND,
                                  wintypes.LONG, wintypes.LONG, wintypes.DWORD,
                                  wintypes.DWORD)
P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
EVENT_SYSTEM_FOREGROUND = 0x0003
WINEVENT_OUTOFCONTEXT = 0x0000
WS_EX_TOPMOST = 0x8
GW_HWNDNEXT = 2
GW_HWNDPREV = 3
BORDER_CLASS = 'WindowMark.WindowBorder'

qpc = ctypes.c_longlong()
k.QueryPerformanceFrequency(ctypes.byref(qpc))


def now_ms():
    c = ctypes.c_longlong()
    k.QueryPerformanceCounter(ctypes.byref(c))
    return c.value * 1000.0 / qpc.value


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


def borders():
    out = []

    def cb(h, _):
        if u.IsWindowVisible(h) and cls(h) == BORDER_CLASS:
            out.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    return out


def find_border(target, pool):
    f = dwm(target) or rc(target)
    cx, cy = (f[0] + f[2]) / 2, (f[1] + f[3]) / 2
    best, bestd = None, 10 ** 9
    for b in pool:
        r = rc(b)
        dist = abs((r[0] + r[2]) / 2 - cx) + abs((r[1] + r[3]) / 2 - cy)
        if dist < bestd:
            bestd, best = dist, b
    return best if bestd <= 80 else None


def above_target(border, target):
    """边框排在目标之上？从边框往下走，碰到目标就是。

    不能用「边框是不是 topmost」当判据：目标有自己的对话框浮着时，边框是**故意**
    落在普通层的（插在对话框之下），那时它照样是到位的。上一版就是因为这个把
    近一半的样本记成了超时。
    """
    h = border
    for _ in range(4096):
        h = u.GetWindow(h, GW_HWNDNEXT)
        if not h:
            return False
        if h == target:
            return True
    return False


appear, vanish = [], []
misses = {'appear': 0, 'vanish': 0}
noborder = {'appear': 0, 'vanish': 0}
lock = threading.Lock()
prev_fg = [u.GetForegroundWindow()]


def watch(target, t0, want_above, bucket, key):
    """盯住一个窗口的边框，等它排到目标之上（出现）或之下（消失）。"""
    deadline = t0 + 1500.0
    border = None
    while now_ms() < deadline:
        if border is None or not u.IsWindow(border):
            border = find_border(target, borders())
            if border is None:
                # 边框还没建出来。EnumWindows 不便宜，隔一下再试，否则这个线程
                # 会把一个核吃满，反过来污染自己要测的延迟。
                time.sleep(0.002)
                continue
        if above_target(border, target) == want_above:
            with lock:
                bucket.append(now_ms() - t0)
            return
        time.sleep(0.001)   # 1ms 分辨率，够细，又不至于忙等
    with lock:
        # 整整 1.5 秒都没找到边框 = 这个窗口根本没有边框（被排除的应用、
        # 系统窗口等），不该算进延迟统计里。
        if border is None:
            noborder[key] += 1
        else:
            misses[key] += 1


def on_foreground(hook, event, hwnd, idobj, idchild, thread, tm):
    if not hwnd or idobj != 0:
        return
    t0 = now_ms()
    old = prev_fg[0]
    prev_fg[0] = hwnd
    if cls(hwnd).startswith('WindowMark.'):
        return
    # 新前台的边框该进 topmost；旧前台的边框该退出来
    threading.Thread(target=watch, args=(hwnd, t0, True, appear, 'appear'),
                     daemon=True).start()
    if old and u.IsWindow(old) and not cls(old).startswith('WindowMark.'):
        threading.Thread(target=watch, args=(old, t0, False, vanish, 'vanish'),
                         daemon=True).start()


proc = WINEVENTPROC(on_foreground)
hook = u.SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, None,
                         proc, 0, 0, WINEVENT_OUTOFCONTEXT)
if not hook:
    print('挂钩子失败')
    raise SystemExit(1)

seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 30

# 自检：把 watch 线程真正会走的那几个函数先跑一遍。
#
# 这些代码只在焦点切换时才执行，而「启动起来不崩」完全覆盖不到——上一版就是这样
# 带着一个 NameError 交出去的，观察 30 秒才发现每个线程都在抛异常。
def selftest():
    fg = u.GetForegroundWindow()
    if not fg:
        return '拿不到前台窗口'
    pool = borders()
    b = find_border(fg, pool)
    if b is not None:
        above_target(b, fg)      # 崩不崩在这一句
    cls(fg)
    pname(fg)
    rc(fg)
    dwm(fg)
    return None


try:
    problem = selftest()
except Exception as exc:                       # noqa: BLE001 - 自检就是要抓住一切
    print('自检没通过：%s: %s' % (type(exc).__name__, exc))
    print('这是工具自己的问题，不是 WindowMark 的。')
    raise SystemExit(3)
if problem:
    print('自检没通过：%s' % problem)
    raise SystemExit(3)

print('观察 %d 秒——请在这段时间里正常来回点击切换窗口（越多次样本越准）。' % seconds)
print('不会抢你的焦点，也不会动任何窗口。')
print()

end = now_ms() + seconds * 1000.0
msg = wintypes.MSG()
while now_ms() < end:
    while u.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
        u.TranslateMessage(ctypes.byref(msg))
        u.DispatchMessageW(ctypes.byref(msg))
    time.sleep(0.002)

u.UnhookWinEvent(hook)
time.sleep(0.4)


def report(name, vals, missed, nb):
    print('=== %s ===' % name)
    if not vals:
        print('  没采到样本（超时 %d 次，无边框的窗口 %d 次）' % (missed, nb))
        return
    vals = sorted(vals)

    def pct(p):
        return vals[min(len(vals) - 1, int(len(vals) * p / 100.0))]

    print('  样本 %d 次，超时 %d 次，跳过（那窗口本来就没边框）%d 次'
          % (len(vals), missed, nb))
    print('  中位数 %.0f ms   p90 %.0f ms   最慢 %.0f ms' % (pct(50), pct(90), vals[-1]))
    for label, limit in (('一帧内(<=17ms)', 16.7), ('三帧内(<=50ms)', 50.0),
                         ('看得出延迟(>100ms)', 100.0)):
        n = sum(1 for v in vals if (v > limit if limit == 100.0 else v <= limit))
        print('  %-20s %d/%d (%.0f%%)' % (label, n, len(vals), 100.0 * n / len(vals)))
    print()


report('边框出现（新前台窗口）', appear, misses['appear'], noborder['appear'])
report('边框消失（旧前台窗口）', vanish, misses['vanish'], noborder['vanish'])
