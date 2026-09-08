# 用 SetWindowPos 模拟一次拖动，采集 WindowMark 的 Redraw 统计。
#
# 真人拖动要人配合，而且每次快慢不同、没法比。程序驱动的移动频率恒定、可重复，改完
# 代码前后拿同一个数比才有意义。
#
# 移动带 SWP_NOACTIVATE | SWP_NOZORDER：不抢焦点、不动 z 序，只改位置——和真实拖动
# 产生的 EVENT_OBJECT_LOCATIONCHANGE 是同一个事件。结束时精确归位。
#
#   py tools\bench-simulated-drag.py            默认 5 秒
#   py tools\bench-simulated-drag.py 8
import ctypes
import io
import os
import sys
import time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND

SWP_NOSIZE = 0x0001
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010

DIR = os.path.join(os.environ['LOCALAPPDATA'], 'WindowMark')
FLAG = os.path.join(DIR, 'diag.on')
LOG = os.path.join(DIR, 'diag.log')


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


def rect(h):
    r = wintypes.RECT()
    u.GetWindowRect(wintypes.HWND(h), ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def move(h, x, y):
    u.SetWindowPos(wintypes.HWND(h), None, x, y, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)


seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0

target = u.GetForegroundWindow()
if not target:
    print('没有前台窗口')
    raise SystemExit(1)
home = rect(target)
print('目标 %s (%s) %s' % (pname(target), cls(target), str(home)))

if os.path.isfile(LOG):
    os.remove(LOG)
with io.open(FLAG, 'w') as fh:
    fh.write('on')
# 诊断开关每秒才生效一次，先空转一下再开始动，免得头一秒白跑。
time.sleep(1.2)

print('模拟拖动 %.0f 秒……' % seconds)
sys.stdout.flush()

# 每 8ms 一步，来回 120px。真实拖动大约就是这个量级：鼠标 125Hz，手速几百像素每秒。
STEP_MS = 0.008
AMPLITUDE = 120
start = time.perf_counter()
moves = 0
while True:
    t = time.perf_counter() - start
    if t >= seconds:
        break
    phase = (t * 1.5) % 2.0            # 1.5 个来回每秒
    offset = int(AMPLITUDE * (phase if phase < 1.0 else 2.0 - phase))
    move(target, home[0] + offset, home[1])
    moves += 1
    time.sleep(STEP_MS)

move(target, home[0], home[1])
time.sleep(1.2)                        # 让最后一秒的统计落盘
try:
    os.remove(FLAG)
except OSError:
    pass

back = rect(target)
print('归位：%s %s' % (str(back), '正确' if back == home else '没回到原处！'))
print('发出 %d 次移动（%.0f 次每秒）' % (moves, moves / seconds))
print()

if not os.path.isfile(LOG):
    print('日志没生成。诊断没打开，或者这段时间边框一次都没重画。')
    raise SystemExit(1)

with io.open(LOG, encoding='utf-8-sig') as fh:
    for line in fh:
        if 'Redraw' in line or 'render 拆开' in line:
            print('  ' + line.rstrip())
