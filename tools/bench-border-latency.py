# 边框跟不跟窗口：造一个自己的测试窗口，挪它，量边框追上来用了多久。
#
# 自己造窗口而不是借用现成的：不动使用者的任何窗口，不抢焦点（SW_SHOWNOACTIVATE），
# 而且每次跑的条件完全一样，数字可以横向比较。
#
# 走的是 EVENT_OBJECT_LOCATIONCHANGE 这条事件路径，量的是「窗口动了 -> 边框就位」
# 这一段端到端延迟。真实拖动还多一层窗口自己的模态循环，那部分归 bench-drag.ps1。
#
#   python tools\bench-border-latency.py           挪 60 次
#   python tools\bench-border-latency.py 200       挪 200 次
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

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wintypes.HWND, wintypes.UINT,
                             wintypes.WPARAM, wintypes.LPARAM)
P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

WS_OVERLAPPEDWINDOW = 0x00CF0000
SW_SHOWNOACTIVATE = 4
SWP_NOSIZE, SWP_NOZORDER, SWP_NOACTIVATE = 0x0001, 0x0004, 0x0010
WM_DESTROY, WM_QUIT = 0x0002, 0x0012
BORDER_CLASS = 'WindowMark.WindowBorder'

qpc = ctypes.c_longlong()
k.QueryPerformanceFrequency(ctypes.byref(qpc))

# 必须显式声明：ctypes 默认按 c_int 推断，而 LPARAM 是 64 位有符号，窗口消息里
# 常见的大值会溢出成 ArgumentError，把窗口过程刷成一屏 traceback。
u.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
                             wintypes.LPARAM]
u.DefWindowProcW.restype = ctypes.c_longlong


def now_ms():
    c = ctypes.c_longlong()
    k.QueryPerformanceCounter(ctypes.byref(c))
    return c.value * 1000.0 / qpc.value


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.UINT), ('style', wintypes.UINT),
                ('lpfnWndProc', WNDPROC), ('cbClsExtra', ctypes.c_int),
                ('cbWndExtra', ctypes.c_int), ('hInstance', wintypes.HINSTANCE),
                ('hIcon', wintypes.HICON), ('hCursor', wintypes.HANDLE),
                ('hbrBackground', wintypes.HBRUSH), ('lpszMenuName', wintypes.LPCWSTR),
                ('lpszClassName', wintypes.LPCWSTR), ('hIconSm', wintypes.HICON)]


def cls(h):
    b = ctypes.create_unicode_buffer(160)
    u.GetClassNameW(h, b, 160)
    return b.value


def rc(h):
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def dwm(h):
    r = wintypes.RECT()
    if d.DwmGetWindowAttribute(wintypes.HWND(h), 9, ctypes.byref(r), ctypes.sizeof(r)) != 0:
        return None
    return (r.left, r.top, r.right, r.bottom)


def enum_borders():
    out = []

    def cb(h, _):
        if u.IsWindowVisible(h) and cls(h) == BORDER_CLASS:
            out.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    return out


def find_border(target):
    f = dwm(target) or rc(target)
    cx, cy = (f[0] + f[2]) / 2, (f[1] + f[3]) / 2
    best, bestd = None, 10 ** 9
    for b in enum_borders():
        r = rc(b)
        dist = abs((r[0] + r[2]) / 2 - cx) + abs((r[1] + r[3]) / 2 - cy)
        if dist < bestd:
            bestd, best = dist, b
    return best if bestd <= 80 else None


def offset(border_rect, frame):
    """边框相对窗口 DWM 边界的外扩量。稳定时四个数相等，用它判断边框到位没有。"""
    return (frame[0] - border_rect[0], frame[1] - border_rect[1],
            border_rect[2] - frame[2], border_rect[3] - frame[3])


def pct(vals, p):
    i = min(len(vals) - 1, int(len(vals) * p / 100.0))
    return vals[i]


rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 60
result = {}


def measure(hwnd):
    # 等 WindowMark 发现这个窗口并给它画边框
    border = None
    for _ in range(60):
        time.sleep(0.1)
        border = find_border(hwnd)
        if border:
            break
    if not border:
        result['error'] = 'WindowMark 没有给测试窗口画边框（6 秒内）'
        u.PostMessageW(hwnd, WM_DESTROY, 0, 0)
        return

    time.sleep(0.3)
    base = offset(rc(border), dwm(hwnd) or rc(hwnd))

    lat, stuck = [], 0
    dx = 60
    for i in range(rounds):
        dx = -dx if (i % 2) else dx
        r = rc(hwnd)
        t0 = now_ms()
        u.SetWindowPos(hwnd, None, r[0] + dx, r[1] + (10 if (i % 4) < 2 else -10), 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
        deadline = t0 + 400.0
        hit = None
        while now_ms() < deadline:
            f = dwm(hwnd) or rc(hwnd)
            if offset(rc(border), f) == base:
                hit = now_ms() - t0
                break
        if hit is None:
            stuck += 1
        else:
            lat.append(hit)
        time.sleep(0.04)

    result['base'] = base
    result['lat'] = sorted(lat)
    result['stuck'] = stuck
    result['border'] = border
    u.PostMessageW(hwnd, WM_DESTROY, 0, 0)


def wndproc(hwnd, msg, wp, lp):
    if msg == WM_DESTROY:
        u.PostQuitMessage(0)
        return 0
    return u.DefWindowProcW(hwnd, msg, wp, lp)


proc = WNDPROC(wndproc)
wc = WNDCLASSEXW()
wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
wc.lpfnWndProc = proc
wc.hInstance = k.GetModuleHandleW(None)
wc.lpszClassName = 'WindowMark.BenchTarget'
wc.hCursor = u.LoadCursorW(None, ctypes.c_wchar_p(32512))
wc.hbrBackground = 6   # COLOR_WINDOW+1
if not u.RegisterClassExW(ctypes.byref(wc)):
    print('注册窗口类失败: %d' % ctypes.get_last_error())
    raise SystemExit(1)

hwnd = u.CreateWindowExW(0, 'WindowMark.BenchTarget', 'WindowMark 边框延迟测试',
                         WS_OVERLAPPEDWINDOW, 300, 200, 700, 480,
                         None, None, wc.hInstance, None)
if not hwnd:
    print('创建窗口失败: %d' % ctypes.get_last_error())
    raise SystemExit(1)

# 不激活地显示：不抢焦点，不打断使用者手上的事
u.ShowWindow(hwnd, SW_SHOWNOACTIVATE)
print('测试窗口已开（不抢焦点），挪 %d 次...' % rounds)

threading.Thread(target=measure, args=(hwnd,), daemon=True).start()

msg = wintypes.MSG()
while u.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
    u.TranslateMessage(ctypes.byref(msg))
    u.DispatchMessageW(ctypes.byref(msg))

u.DestroyWindow(hwnd)

if 'error' in result:
    print(result['error'])
    raise SystemExit(2)

lat, stuck = result['lat'], result['stuck']
print('稳定时的外扩量（左上右下）：%s' % str(result['base']))
print()
if not lat:
    print('全部超时，边框没有跟随。')
    raise SystemExit(2)

print('=== 边框追上窗口所需时间（%d 次移动）===' % rounds)
print('  中位数 %.1f ms   p90 %.1f ms   p99 %.1f ms   最慢 %.1f ms'
      % (pct(lat, 50), pct(lat, 90), pct(lat, 99), lat[-1]))
print('  超时(>400ms) %d 次' % stuck)
print()
# 60Hz 一帧 16.7ms。跟手与否的分界线在这儿：一帧之内追上，眼睛看不出边框是独立的
# 窗口；到两三帧就能看出边框在拖后腿。
for label, limit in (('一帧内(<=16.7ms)', 16.7), ('两帧内(<=33.4ms)', 33.4)):
    n = sum(1 for v in lat if v <= limit)
    print('  %s追上：%d/%d (%.0f%%)' % (label, n, len(lat), 100.0 * n / len(lat)))
