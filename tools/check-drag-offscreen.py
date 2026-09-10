# 窗口飞出屏幕只剩一个角，能不能用「修饰键 + 左键」从任意位置拖回来。
#
# 这是这个功能最主要的用途：窗口跑到屏幕外，标题栏和四条边都够不着，鼠标能碰到的只有
# 露在屏幕上的那一小块。按住修饰键在**那一小块的任意位置**按下左键就该能拖——不是只有
# 标题栏、也不分窗口内的区域，整窗跟着光标走。
#
# **拖动一定要用相对位移注入**（MOUSEEVENTF_MOVE 不带 ABSOLUTE），因为真实鼠标就是这么
# 报的。第一版用绝对坐标，结果放过了一个真实的 bug：钩子当时吞掉了 WM_MOUSEMOVE，光标
# 被钉死，而绝对坐标的 pt 不依赖当前光标位置，照样累加，测试一路绿灯——手上却拖不动，
# 窗口只晃一下。判据也跟着改了：比的是「窗口位移 == 光标真实位移」，不是「窗口位移 ==
# 我注入的位移」，后者会被指针加速带偏。
#
#   py tools\check-drag-offscreen.py
import ctypes
import io
import os
import subprocess
import sys
import time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND
u.GetTopWindow.restype = wintypes.HWND
u.GetWindow.restype = wintypes.HWND
u.FindWindowW.restype = wintypes.HWND
u.WindowFromPoint.restype = wintypes.HWND
u.WindowFromPoint.argtypes = [wintypes.POINT]
u.GetAncestor.restype = wintypes.HWND
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

CONF = os.path.join(os.environ['LOCALAPPDATA'], 'WindowMark', 'settings.conf')
EXE = os.path.join(os.environ['LOCALAPPDATA'], 'Programs', 'WindowMark', 'WindowMark.exe')

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
# 0x2000 是 MOVE_NOCOALESCE，0x4000 才是 VIRTUALDESK。第一版把两个记混了，绝对坐标
# 按主屏而不是整个虚拟桌面换算，光标落点整整偏了 240px——而 y 正好没露馅，因为虚拟
# 桌面的高度和主屏一样是 1440。差点当成功能的 bug。
MOUSEEVENTF_MOVE_NOCOALESCE = 0x2000
MOUSEEVENTF_VIRTUALDESK = 0x4000
MOUSEEVENTF_ABSOLUTE = 0x8000
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_EXTENDEDKEY = 0x0001
VK_RMENU = 0xA5
HWND_TOP = wintypes.HWND(0)
SWP_NOSIZE = 0x0001
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [('dx', wintypes.LONG), ('dy', wintypes.LONG),
                ('mouseData', wintypes.DWORD), ('dwFlags', wintypes.DWORD),
                ('time', wintypes.DWORD), ('dwExtraInfo', ctypes.POINTER(wintypes.ULONG))]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [('wVk', wintypes.WORD), ('wScan', wintypes.WORD),
                ('dwFlags', wintypes.DWORD), ('time', wintypes.DWORD),
                ('dwExtraInfo', ctypes.POINTER(wintypes.ULONG))]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [('mi', MOUSEINPUT), ('ki', KEYBDINPUT)]

    _anonymous_ = ('u',)
    _fields_ = [('type', wintypes.DWORD), ('u', _U)]


VX, VY = u.GetSystemMetrics(76), u.GetSystemMetrics(77)
VW, VH = u.GetSystemMetrics(78), u.GetSystemMetrics(79)
SW, SH = u.GetSystemMetrics(0), u.GetSystemMetrics(1)


def send(inp):
    u.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def key(vk, up):
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp.ki = KEYBDINPUT(vk, 0, KEYEVENTF_EXTENDEDKEY | (KEYEVENTF_KEYUP if up else 0),
                        0, None)
    send(inp)


def mouse_flag(flag):
    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.mi = MOUSEINPUT(0, 0, 0, flag, 0, None)
    send(inp)


def move_abs(x, y):
    # 只在拖动开始前定位用。绝对坐标是 0..65535 归一化到整个虚拟桌面的。
    nx = int((x - VX) * 65535 / max(1, VW - 1))
    ny = int((y - VY) * 65535 / max(1, VH - 1))
    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.mi = MOUSEINPUT(nx, ny, 0,
                        MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                        MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE, 0, None)
    send(inp)


def move_rel(dx, dy):
    # 拖动过程中只用这个：真实鼠标报的就是相对位移。
    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.mi = MOUSEINPUT(dx, dy, 0, MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE, 0, None)
    send(inp)


def cursor():
    p = wintypes.POINT()
    u.GetCursorPos(ctypes.byref(p))
    return (p.x, p.y)


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


# 程序不能拖的那些。锁屏那两个尤其要点名：解锁之后有时不消失，topmost、铺满整块屏、
# IsWindowVisible 还是 1，WindowFromPoint 一问就是它们。
UNDRAGGABLE = ('Progman', 'WorkerW', 'Shell_TrayWnd', 'Shell_SecondaryTrayWnd',
               'Windows.UI.Core.CoreWindow', 'LockScreenInputOcclusionFrame')


def rect(h):
    r = wintypes.RECT()
    u.GetWindowRect(wintypes.HWND(h), ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def topmost_draggable(x, y):
    """盖住 (x, y) 的、z 序最靠前的可拖窗口。对应程序里 TargetWindowAt 的下探逻辑。"""
    h = u.GetTopWindow(None)
    guard = 0
    while h and guard < 4096:
        guard += 1
        name = cls(h)
        if u.IsWindowVisible(h) and not u.IsIconic(h) \
                and not name.startswith('WindowMark.') and name not in UNDRAGGABLE:
            r = rect(h)
            if r[0] <= x < r[2] and r[1] <= y < r[3]:
                return h
        h = u.GetWindow(h, 2)
    return None


def read_key(name):
    with io.open(CONF, encoding='utf-8') as fh:
        for line in fh:
            if line.startswith(name + '='):
                return line.strip()[len(name) + 1:]
    return ''


def write_keys(pairs):
    subprocess.run(['taskkill', '/IM', 'WindowMark.exe', '/F'], capture_output=True)
    time.sleep(1.5)
    with io.open(CONF, encoding='utf-8') as fh:
        lines = fh.readlines()
    out = []
    for line in lines:
        hit = None
        for name, value in pairs.items():
            if line.startswith(name + '='):
                hit = '%s=%s\n' % (name, value)
                break
        out.append(hit if hit else line)
    with io.open(CONF, 'w', encoding='utf-8', newline='') as fh:
        fh.writelines(out)
    subprocess.Popen([EXE], close_fds=True)
    time.sleep(3.0)


# 挑一个不是前台的普通窗口，免得动到用户正在用的那个
target = None
fg = u.GetForegroundWindow()
h = u.GetTopWindow(None)
guard = 0
while h and guard < 4096:
    guard += 1
    name = cls(h)
    if u.IsWindowVisible(h) and not u.IsIconic(h) and not name.startswith('WindowMark.') \
            and name not in ('Progman', 'WorkerW', 'Shell_TrayWnd', 'Shell_SecondaryTrayWnd') \
            and int(h) != int(fg) and u.IsZoomed(h) == 0:
        r = rect(h)
        if r[2] - r[0] > 400 and r[3] - r[1] > 300:
            target = h
            break
    h = u.GetWindow(h, 2)

if not target:
    print('找不到合适的窗口')
    raise SystemExit(1)

original = {'drag.enabled': read_key('drag.enabled'),
            'drag.modifiers': read_key('drag.modifiers')}
home = rect(target)
home_cursor = cursor()


def restore():
    u.SetWindowPos(wintypes.HWND(target), None, home[0], home[1], 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
    u.SetCursorPos(*home_cursor)
    write_keys(original)
    print('已恢复窗口位置和 drag 配置。')


print('目标窗口：%s (%s) %s' % (pname(target), cls(target), str(home)))
print('打开拖动，触发键设成 RAlt……')
write_keys({'drag.enabled': 'true', 'drag.modifiers': 'RAlt'})

# 逐块屏去试：把窗口推到那块屏的右下角只剩一小块，再确认按下点解析出来的确实是它。
#
# 有别的 topmost 窗口盖着时，WindowFromPoint 拿到的是压在上面那个，TargetWindowAt
# 返回空，拖动根本不会启动——不验这个前提，脚本会把它当成「拖不动」，冤枉功能。
# 本机就撞上过：一个 LockApp.exe 的锁屏窗口铺满主屏、topmost、IsWindowVisible=1，
# 屏幕明明没锁它却赖着不走。
monitors = []
MI_CB = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC,
                           ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)


class MONITORINFO(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.DWORD), ('rcMonitor', wintypes.RECT),
                ('rcWork', wintypes.RECT), ('dwFlags', wintypes.DWORD)]


def collect(mon, hdc, lprc, data):
    mi = MONITORINFO()
    mi.cbSize = ctypes.sizeof(MONITORINFO)
    u.GetMonitorInfoW(mon, ctypes.byref(mi))
    m = mi.rcMonitor
    monitors.append(((m.left, m.top, m.right, m.bottom), bool(mi.dwFlags & 1)))
    return True


u.EnumDisplayMonitors(None, None, MI_CB(collect), 0)
monitors.sort(key=lambda entry: not entry[1])   # 先试主屏

stranded = None
grabX = grabY = 0
screen = None
blocked = []
for area, isPrimary in monitors:
    u.SetWindowPos(wintypes.HWND(target), HWND_TOP, area[2] - 120, area[3] - 120, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE)
    time.sleep(0.7)
    r = rect(target)
    gx, gy = r[0] + 60, r[1] + 60
    under = topmost_draggable(gx, gy)
    if under and int(under) == int(target):
        stranded, grabX, grabY, screen = r, gx, gy, area
        print('用这块屏：%s%s' % (str(area), '（主屏）' if isPrimary else ''))
        break
    blocked.append('%s 的按下点上，最靠前的可拖窗口是 %s'
                   % (str(area), cls(under) if under else 'None'))

if stranded is None:
    print('！！每块屏的按下点都被别的窗口盖着，没法测：')
    for b in blocked:
        print('   ' + b)
    restore()
    sys.exit(1)
# 挪窗口会让 WindowMark 重扫 z 序、重画两块屏的边框。WH_MOUSE_LL 的回调就跑在那条
# 线程上，它忙着的时候鼠标事件会撞上 LowLevelHooksTimeout 被系统直接放行——头几步
# 拖不动完全是这么来的，和拖动逻辑无关。等它闲下来再开始。
time.sleep(2.0)
print('推出屏幕后：%s（右下只剩约 120x120 的一角）' % str(stranded))

# 在露出来的那一角正中按下——不是标题栏，就是窗口内容区
move_abs(grabX, grabY)
time.sleep(0.4)
landed = cursor()
if abs(landed[0] - grabX) > 2 or abs(landed[1] - grabY) > 2:
    print('！！光标没落到 (%d, %d)，实际 %s——是脚本的坐标换算不对，不是功能的问题'
          % (grabX, grabY, str(landed)))
    restore()
    sys.exit(1)

print('在 %s 按住 RAlt + 左键，用相对位移往这块屏中间拖……' % str(landed))
key(VK_RMENU, False)
time.sleep(0.2)
mouse_flag(MOUSEEVENTF_LEFTDOWN)
time.sleep(0.25)

winBefore = rect(target)
curBefore = cursor()

# 自适应步进：每步读一次实际光标，按剩余距离收敛，绝不冲出桌面边界。
#
# 固定步长会被指针加速放大（本机实测 40px 的注入位移变成约 80px），光标一路冲到角上被
# 系统夹住，而钩子收到的 pt 是未夹取的值，窗口就比光标多走一步——那是脚本过冲，会盖住
# 真正要看的差异。
destX = (screen[0] + screen[2]) // 2
destY = (screen[1] + screen[3]) // 2
wantX, wantY = destX - curBefore[0], destY - curBefore[1]
for _ in range(80):
    at = cursor()
    dx, dy = destX - at[0], destY - at[1]
    if abs(dx) <= 3 and abs(dy) <= 3:
        break
    # 每步最多走剩余的一半，且不超过 40px：加速再怎么放大也越不过目的地。
    move_rel(max(-40, min(40, dx // 2)) or (1 if dx > 0 else -1 if dx < 0 else 0),
             max(-40, min(40, dy // 2)) or (1 if dy > 0 else -1 if dy < 0 else 0))
    time.sleep(0.02)
time.sleep(0.3)

winAfter = rect(target)
curAfter = cursor()
mouse_flag(MOUSEEVENTF_LEFTUP)
time.sleep(0.2)
key(VK_RMENU, True)
time.sleep(0.5)

winMoved = (winAfter[0] - winBefore[0], winAfter[1] - winBefore[1])
curMoved = (curAfter[0] - curBefore[0], curAfter[1] - curBefore[1])
print()
print('光标 %s -> %s，位移 %s' % (str(curBefore), str(curAfter), str(curMoved)))
print('窗口 %s -> %s，位移 %s' % (str(winBefore[:2]), str(winAfter[:2]), str(winMoved)))

restore()
print()

failures = []

# 1. 光标真的动了。少了这一条，「窗口没动 + 光标也没动」会当成两者一致而蒙混过关——
#    而那恰恰就是吞掉 WM_MOUSEMOVE 时的样子。
if abs(curMoved[0]) < abs(wantX) // 2 or abs(curMoved[1]) < abs(wantY) // 2:
    failures.append('光标几乎没动（想走 %s，实走 %s）。移动事件被吞了，'
                    '真实鼠标下位移永远累加不起来。' % (str((wantX, wantY)), str(curMoved)))

# 2. 窗口跟着光标走，位移一致
if abs(winMoved[0] - curMoved[0]) > 8 or abs(winMoved[1] - curMoved[1]) > 8:
    failures.append('窗口没跟上光标，差 (%d, %d)。'
                    % (winMoved[0] - curMoved[0], winMoved[1] - curMoved[1]))

# 3. 用户真正在意的结果：窗口回到屏幕里了
visible = max(0, min(winAfter[2], screen[2]) - max(winAfter[0], screen[0])) * \
          max(0, min(winAfter[3], screen[3]) - max(winAfter[1], screen[1]))
whole = (winAfter[2] - winAfter[0]) * (winAfter[3] - winAfter[1])
if whole <= 0 or visible < whole * 0.5:
    failures.append('窗口还是大半在屏幕外，救援没成功。')

if failures:
    print('！！%d 项不对：' % len(failures))
    for f in failures:
        print('  - ' + f)
    sys.exit(1)

print('成功：光标走了 %s，窗口位移一致，窗口已回到屏幕内。' % str(curMoved))
print('按下的位置是窗口内容区，不是标题栏——「窗口内任意位置都能拖」成立。')
