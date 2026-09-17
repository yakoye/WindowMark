# 列出边框规划器眼里的「遮挡物」：快照会收、而且会拿去裁别人边框的那些窗口。
#
# v0.4.9 起边框改成每屏一张 overlay，谁盖住谁由 WindowMark 自己算：快照里排在前面、
# 可见、没被 cloak 的窗口一律当不透明遮挡物。只要混进一个「系统说它可见、屏幕上却看
# 不见」的大窗口，下面所有边框都会被裁光——而 v0.4.8 不算遮挡，z 序交给系统，所以免疫。
#
# 每个窗口额外报三件事，都是判断「它到底画没画在桌面上」的线索：
#   band      GetWindowBand。1 = ZBID_DESKTOP，普通桌面层。锁屏、开始菜单这类沉浸式界面
#             在别的 band，和桌面窗口根本不在同一个叠放序列里，GetTopWindow 却会把它们
#             混在一起吐出来。
#   cloak     DWMWA_CLOAKED，**带 HRESULT**。调用失败时变量保持 0，不查返回值就会把
#             「问不出来」当成「没被藏」。
#   exstyle   WS_EX_TRANSPARENT / LAYERED / NOACTIVATE / TOOLWINDOW。
#
#   py tools\list-occluders.py
import ctypes
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)
u.GetTopWindow.restype = wintypes.HWND
u.GetWindow.restype = wintypes.HWND
u.GetForegroundWindow.restype = wintypes.HWND
u.GetShellWindow.restype = wintypes.HWND
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

try:
    GetWindowBand = u.GetWindowBand
    GetWindowBand.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    GetWindowBand.restype = wintypes.BOOL
except AttributeError:
    GetWindowBand = None

BANDS = {0: 'DEFAULT', 1: 'DESKTOP', 2: 'UIACCESS', 3: 'IMM_IHM', 4: 'IMM_NOTIF',
         5: 'IMM_APPCHROME', 6: 'IMM_MOGO', 7: 'IMM_EDGY', 8: 'IMM_INACTIVEMOBODY',
         9: 'IMM_INACTIVEDOCK', 10: 'IMM_ACTIVEMOBODY', 11: 'IMM_ACTIVEDOCK',
         12: 'IMM_BACKGROUND', 13: 'IMM_SEARCH', 14: 'GENUINE_WINDOWS',
         15: 'IMM_RESTRICTED', 16: 'SYSTEM_TOOLS', 17: 'LOCK', 18: 'ABOVELOCK_UX'}


def cls(h):
    b = ctypes.create_unicode_buffer(160)
    u.GetClassNameW(h, b, 160)
    return b.value


def title(h):
    b = ctypes.create_unicode_buffer(160)
    u.GetWindowTextW(h, b, 160)
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
        u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def cloak(h):
    c = wintypes.DWORD(0)
    hr = d.DwmGetWindowAttribute(wintypes.HWND(h), 14, ctypes.byref(c), ctypes.sizeof(c))
    if hr != 0:
        return '失败 0x%08X' % (hr & 0xFFFFFFFF)
    return str(c.value)


def band(h):
    if GetWindowBand is None:
        return 'n/a'
    b = wintypes.DWORD(0)
    if not GetWindowBand(h, ctypes.byref(b)):
        return '失败'
    return '%d %s' % (b.value, BANDS.get(b.value, ''))


fg = u.GetForegroundWindow()
shell = u.GetShellWindow()
screen_area = u.GetSystemMetrics(78) * u.GetSystemMetrics(79)

print('前台：%s | %s' % (cls(fg), title(fg)[:40]))
print()
print('%-3s %-34s %-16s %-10s %-16s %-6s %-22s %s'
      % ('#', '类名', '进程', '占屏', 'band', 'cloak', 'exstyle', '备注'))

h = u.GetTopWindow(None)
n = 0
guard = 0
while h and guard < 4096:
    guard += 1
    if u.IsWindowVisible(h) and not cls(h).startswith('WindowMark.'):
        f = frame(h)
        area = max(0, f[2] - f[0]) * max(0, f[3] - f[1])
        if area > 0:
            n += 1
            ex = u.GetWindowLongW(h, -20) & 0xFFFFFFFF
            flags = []
            if ex & 0x8:
                flags.append('TOPMOST')
            if ex & 0x20:
                flags.append('TRANSP')
            if ex & 0x80000:
                flags.append('LAYERED')
            if ex & 0x8000000:
                flags.append('NOACT')
            if ex & 0x80:
                flags.append('TOOL')
            share = area * 100.0 / max(1, screen_area)
            note = []
            if int(h) == int(fg):
                note.append('<- 前台')
            if int(h) == int(shell):
                note.append('桌面')
            b = band(h)
            if share >= 50 and not b.startswith('1 '):
                note.append('!! 大窗口且不在桌面 band')
            elif share >= 50 and 'TOPMOST' in flags:
                note.append('!! 大的置顶窗口')
            print('%-3d %-34s %-16s %5.1f%%    %-16s %-6s %-22s %s'
                  % (n, cls(h)[:34], pname(h)[:16], share, b, cloak(h),
                     ','.join(flags), ' '.join(note)))
    h = u.GetWindow(h, 2)
