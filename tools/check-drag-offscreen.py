# 窗口飞出屏幕只剩一个角，能不能用「修饰键 + 左键」从任意位置拖回来。
#
# 这是这个功能最主要的用途：窗口跑到屏幕外，标题栏和四条边都够不着，鼠标能碰到的只有
# 露在屏幕上的那一小块。按住修饰键在**那一小块的任意位置**按下左键就该能拖——不是只有
# 标题栏、也不分窗口内的区域，整窗跟着光标走。
#
# 做法：把一个窗口推到右下角只剩一小块，合成「按住 RAlt + 左键拖」，看它回不回来。
# 全程用 SendInput，光标会自己动；跑完把窗口和光标都放回原处。
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
d = ctypes.WinDLL('dwmapi', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND
u.GetTopWindow.restype = wintypes.HWND
u.GetWindow.restype = wintypes.HWND
u.FindWindowW.restype = wintypes.HWND
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

DIR = os.path.join(os.environ['LOCALAPPDATA'], 'WindowMark')
CONF = os.path.join(DIR, 'settings.conf')
EXE = os.path.join(os.environ['LOCALAPPDATA'], 'Programs', 'WindowMark', 'WindowMark.exe')

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_EXTENDEDKEY = 0x0001
# 0x2000 是 MOVE_NOCOALESCE，0x4000 才是 VIRTUALDESK。第一版把两个记混了，绝对坐标
# 按主屏而不是整个虚拟桌面换算，光标落点整整偏了 240px——而 y 正好没露馅，因为虚拟
# 桌面的高度和主屏一样是 1440。差点当成功能的 bug。
MOUSEEVENTF_MOVE_NOCOALESCE = 0x2000
MOUSEEVENTF_VIRTUALDESK = 0x4000
VK_RMENU = 0xA5
SWP_NOSIZE = 0x0001
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


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


def send(inp):
    u.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def key(vk, up):
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    flags = KEYEVENTF_EXTENDEDKEY | (KEYEVENTF_KEYUP if up else 0)
    inp.ki = KEYBDINPUT(vk, 0, flags, 0, None)
    send(inp)


def mouse_flag(flag):
    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.mi = MOUSEINPUT(0, 0, 0, flag, 0, None)
    send(inp)


def move_to(x, y):
    # 绝对坐标是 0..65535 归一化到整个虚拟桌面的
    vx = u.GetSystemMetrics(76)
    vy = u.GetSystemMetrics(77)
    vw = u.GetSystemMetrics(78)
    vh = u.GetSystemMetrics(79)
    nx = int((x - vx) * 65535 / max(1, vw - 1))
    ny = int((y - vy) * 65535 / max(1, vh - 1))
    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.mi = MOUSEINPUT(nx, ny, 0,
                        MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                        MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE, 0, None)
    send(inp)


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
        done = False
        for name, value in pairs.items():
            if line.startswith(name + '='):
                out.append('%s=%s\n' % (name, value))
                done = True
                break
        if not done:
            out.append(line)
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
cursor = wintypes.POINT()
u.GetCursorPos(ctypes.byref(cursor))

print('目标窗口：%s (%s) %s' % (pname(target), cls(target), str(home)))
print('打开拖动，触发键设成 RAlt……')
write_keys({'drag.enabled': 'true', 'drag.modifiers': 'RAlt'})

# 推到右下角，只剩左上角一小块在屏幕里
sw = u.GetSystemMetrics(78) or u.GetSystemMetrics(0)
sh = u.GetSystemMetrics(79) or u.GetSystemMetrics(1)
vx = u.GetSystemMetrics(76)
vy = u.GetSystemMetrics(77)
offX = vx + sw - 120
offY = vy + sh - 120
u.SetWindowPos(wintypes.HWND(target), None, offX, offY, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
# 挪窗口会让 WindowMark 重扫 z 序、重画两块屏的边框。WH_MOUSE_LL 的回调就跑在那条
# 线程上，它忙着的时候鼠标事件会撞上 LowLevelHooksTimeout 被系统直接放行——头几步
# 拖不动完全是这么来的，和拖动逻辑无关。等它闲下来再开始。
time.sleep(2.5)
stranded = rect(target)
print('推出屏幕后：%s（屏幕右下只剩约 120x120 的一角）' % str(stranded))

# 在露出来的那一角正中按下——不是标题栏，就是窗口内容区
grabX = stranded[0] + 60
grabY = stranded[1] + 60
targetX = vx + sw // 2
targetY = vy + sh // 2
print('在 (%d, %d) 按住 RAlt + 左键，拖到 (%d, %d)……' % (grabX, grabY, targetX, targetY))

move_to(grabX, grabY)
time.sleep(0.3)
# 先确认光标真的到了要去的地方。不查这一步，脚本自己的坐标算错会伪装成「窗口没跟上」。
landed = wintypes.POINT()
u.GetCursorPos(ctypes.byref(landed))
if abs(landed.x - grabX) > 2 or abs(landed.y - grabY) > 2:
    print('！！光标没落到 (%d, %d)，实际 (%d, %d)——是脚本的坐标换算不对，不是功能的问题'
          % (grabX, grabY, landed.x, landed.y))
    u.SetWindowPos(wintypes.HWND(target), None, home[0], home[1], 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
    write_keys(original)
    sys.exit(1)
key(VK_RMENU, False)
time.sleep(0.2)
mouse_flag(MOUSEEVENTF_LEFTDOWN)
time.sleep(0.2)
# 分几步移动，模拟真实拖动
for i in range(1, 11):
    move_to(grabX + (targetX - grabX) * i // 10, grabY + (targetY - grabY) * i // 10)
    time.sleep(0.05)
mouse_flag(MOUSEEVENTF_LEFTUP)
time.sleep(0.2)
key(VK_RMENU, True)
time.sleep(0.6)

after = rect(target)
print('拖动后：%s' % str(after))

moved = (after[0] - stranded[0], after[1] - stranded[1])
wanted = (targetX - grabX, targetY - grabY)
print('窗口位移 %s，光标位移 %s' % (str(moved), str(wanted)))

# 还原
u.SetWindowPos(wintypes.HWND(target), None, home[0], home[1], 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
u.SetCursorPos(cursor.x, cursor.y)
write_keys(original)
print()
print('已恢复窗口位置和 drag 配置。')
print()

tolerance = 8
ok = abs(moved[0] - wanted[0]) <= tolerance and abs(moved[1] - wanted[1]) <= tolerance
if ok:
    print('成功：窗口跟着光标回到了屏幕中间，位移与光标一致（误差 %d px 以内）。'
          % tolerance)
    print('按下的位置是窗口内容区，不是标题栏——「窗口内任意位置都能拖」成立。')
else:
    print('！！窗口没跟上。位移差 (%d, %d)。'
          % (moved[0] - wanted[0], moved[1] - wanted[1]))
    sys.exit(1)
