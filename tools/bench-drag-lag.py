# 量拖动时边框落后窗口多少像素。
#
# 「不跟手」是个感觉，滞后像素数是它的数字形式。做法：一边高频读窗口矩形，一边抓一条
# 横跨左边界的像素条找出边框实际画在哪，两者相减。
#
# 两个让采样率上得去的选择：
#   - 用 GetWindowRect 而不是 DWM 的 EXTENDED_FRAME_BOUNDS。后者是跨进程调用，实测
#     一次采样要 16ms，量出来的全是工具自己的延迟。前者含一圈不可见的 resize border
#     （125% 下 8px），但那是个固定偏移，测「滞后」这种相对量不受影响。
#   - 抓 120x1 的一条而不是整屏。整屏一次 14MB。
#
#   py tools\bench-drag-lag.py             等你自己拖
#   py tools\bench-drag-lag.py --simulate  脚本自己拖，可重复、可前后对比
import ctypes
import sys
import threading
import time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
g = ctypes.WinDLL('gdi32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

SRCCOPY = 0x00CC0020
ACTIVE = (0x62, 0x74, 0xE7)
INACTIVE = (0x70, 0x80, 0xAA)
SPAN = 60
MEASURE_SECONDS = 5.0
SWP_NOSIZE = 0x0001
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010


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


_rect = wintypes.RECT()


def rect(h):
    u.GetWindowRect(wintypes.HWND(h), ctypes.byref(_rect))
    return (_rect.left, _rect.top, _rect.right, _rect.bottom)


screen_dc = u.GetDC(0)
mem_dc = g.CreateCompatibleDC(screen_dc)
bmp = g.CreateCompatibleBitmap(screen_dc, SPAN * 2, 1)
g.SelectObject(mem_dc, bmp)
info = BITMAPINFO()
info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
info.bmiHeader.biWidth = SPAN * 2
info.bmiHeader.biHeight = -1
info.bmiHeader.biPlanes = 1
info.bmiHeader.biBitCount = 32
strip = ctypes.create_string_buffer(SPAN * 2 * 4)


def outer_edge(x0, y):
    """抓一行，返回最靠左的边框色像素的 x；没有就 None。"""
    g.BitBlt(mem_dc, 0, 0, SPAN * 2, 1, screen_dc, x0, y, SRCCOPY)
    g.GetDIBits(mem_dc, bmp, 0, 1, strip, ctypes.byref(info), 0)
    for i in range(SPAN * 2):
        off = i * 4
        p = (strip[off + 2][0], strip[off + 1][0], strip[off][0])
        if any(all(abs(p[j] - ref[j]) <= 12 for j in range(3)) for ref in (ACTIVE, INACTIVE)):
            return x0 + i
    return None


simulate = '--simulate' in sys.argv

if simulate:
    target = u.GetForegroundWindow()
    if not target:
        print('没有前台窗口')
        raise SystemExit(1)
    home = rect(target)
    print('模拟拖动 %s (%s)' % (pname(target), cls(target)))
    stop = threading.Event()

    def driver():
        # 和 bench-simulated-drag.py 同一套参数，两个工具的数才好互相印证。
        start = time.perf_counter()
        while not stop.is_set():
            t = time.perf_counter() - start
            phase = (t * 1.5) % 2.0
            offset = int(120 * (phase if phase < 1.0 else 2.0 - phase))
            u.SetWindowPos(wintypes.HWND(target), None, home[0] + offset, home[1], 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
            time.sleep(0.008)

    thread = threading.Thread(target=driver, daemon=True)
    thread.start()
    time.sleep(0.3)
else:
    print('按住任意窗口的标题栏开始拖动，检测到就开始计时，量 %.0f 秒。' % MEASURE_SECONDS)
    print('（不用急，等你拖起来才开始）')
    sys.stdout.flush()

    target = None
    prev = {}
    deadline = time.perf_counter() + 60.0
    while time.perf_counter() < deadline:
        fg = u.GetForegroundWindow()
        if not fg:
            time.sleep(0.02)
            continue
        r = rect(fg)
        key = int(fg)
        if key in prev and prev[key] != r and prev[key][0] != r[0]:
            target = fg
            break
        prev[key] = r
        time.sleep(0.01)

    if target is None:
        print('等了 60 秒没等到拖动。')
        raise SystemExit(1)
    home = None
    print()
    print('开始：%s (%s)' % (pname(target), cls(target)))
    sys.stdout.flush()

# 不要求「这次位置和上次不同」才计数。采样比窗口移动快得多，那样过滤会把绝大多数样本
# 丢掉。静止时的样本自然落在基准值上，动起来才拉开距离——两种都留着，分布本身说明问题。
samples = []
misses = 0
n = 0
positions = set()
t_end = time.perf_counter() + MEASURE_SECONDS
while time.perf_counter() < t_end:
    r0 = rect(target)
    y = (r0[1] + r0[3]) // 2
    edge = outer_edge(r0[0] - SPAN, y)
    r1 = rect(target)
    n += 1
    if r0 != r1:
        continue                 # 采样期间窗口动了，这一次的坐标对不上，作废
    positions.add(r0[0])
    if edge is None:
        misses += 1
    else:
        samples.append(r0[0] - edge)

if simulate:
    stop.set()
    time.sleep(0.1)
    u.SetWindowPos(wintypes.HWND(target), None, home[0], home[1], 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
    back = rect(target)
    print('归位：%s' % ('正确' if back == home else '没回到原处 %s' % str(back)))

print()
print('采样 %d 次（%.0f 次每秒），有效 %d 个，其中找不到边框 %d 个'
      % (n, n / MEASURE_SECONDS, len(samples) + misses, misses))
print('采到过 %d 个不同的窗口位置——这个数小就说明窗口其实没怎么动，结论不作数'
      % len(positions))

if not samples:
    print('没有可用样本。')
    raise SystemExit(0)

samples.sort()


def pct(p):
    return samples[min(len(samples) - 1, int(len(samples) * p))]


base = samples[0]
print()
print('窗口左边 − 边框外沿（静止时是个常数，边框落后时变大）：')
print('  最小 %d   中位 %d   p90 %d   p99 %d   最大 %d'
      % (base, pct(0.5), pct(0.9), pct(0.99), samples[-1]))
print()
print('滞后像素（减掉最小值这个基准）：')
print('  中位 %d   p90 %d   p99 %d   最大 %d'
      % (pct(0.5) - base, pct(0.9) - base, pct(0.99) - base, samples[-1] - base))
print('  边框整条没画出来的采样：%d 次（%.0f%%）'
      % (misses, 100.0 * misses / (len(samples) + misses)))

g.DeleteObject(bmp)
g.DeleteDC(mem_dc)
u.ReleaseDC(0, screen_dc)
