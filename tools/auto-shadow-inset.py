# 自动量出一个窗口的矩形比它看得见的部分大多少。
#
# Windows 11 的弹出面板（托盘溢出、任务栏缩略图之类）把阴影画在自己的窗口矩形里，
# 矩形比看得见的那块大二三十像素，没有任何 API 说得出大多少。结果是别人的边框在它
# 旁边断开的那一截明显比它本身宽——看着像边框断了，而不是被它挡住。
#
# 量法：沿垂直于边的方向取一条像素，从窗口外一路扫进去。阴影是渐变的，实体的边缘是
# 突变，所以**梯度最大**的那个位置就是可见边缘。四条边各取若干位置，取中位数，避开
# 面板上恰好有深色图标的那些列。
#
#   py tools\auto-shadow-inset.py               鼠标停在要测的窗口上，五秒后开始
#   py tools\auto-shadow-inset.py overflow      按进程名或类名找
#   py tools\auto-shadow-inset.py --foreground  测当前前台窗口
#   加 --apply 就直接写进配置并重启 WindowMark，不用手抄
import ctypes
import sys
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
g = ctypes.WinDLL('gdi32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)

u.GetForegroundWindow.restype = wintypes.HWND
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

SRCCOPY = 0x00CC0020
P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
REACH = 48          # 往窗口里最多找这么深


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


def frame(h):
    r = wintypes.RECT()
    if d.DwmGetWindowAttribute(wintypes.HWND(h), 9, ctypes.byref(r), ctypes.sizeof(r)) != 0:
        u.GetWindowRect(wintypes.HWND(h), ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


u.WindowFromPoint.restype = wintypes.HWND
u.WindowFromPoint.argtypes = [wintypes.POINT]
u.GetAncestor.restype = wintypes.HWND

args = [a for a in sys.argv[1:] if not a.startswith('-')]
if '--foreground' in sys.argv:
    target = u.GetForegroundWindow()
elif not args:
    # 没给关键字就用鼠标指的那个。弹出面板往往没法用关键字找——它一失去焦点就关了，
    # 而鼠标停在上面它还在。
    import time as _t
    print('把鼠标移到要测的窗口上，别动。5 秒后开始。')
    for left in range(5, 0, -1):
        print('  %d...' % left)
        sys.stdout.flush()
        _t.sleep(1)
    pt = wintypes.POINT()
    u.GetCursorPos(ctypes.byref(pt))
    hit = u.WindowFromPoint(pt)
    target = u.GetAncestor(hit, 2) if hit else None
    print()
else:
    want = args[0].lower()
    found = []

    def cb(h, _):
        if u.IsWindowVisible(h) and not u.IsIconic(h) and not cls(h).startswith('WindowMark.'):
            r = frame(h)
            if r[2] - r[0] > 80 and r[3] - r[1] > 60:
                if want in pname(h).lower() or want in cls(h).lower():
                    found.append(h)
        return True

    u.EnumWindows(P(cb), 0)
    target = found[0] if found else None

if not target:
    print('找不到窗口')
    raise SystemExit(1)

f = frame(target)
name = cls(target)
print('%s (%s) %s' % (pname(target), name, str(f)))
print('尺寸 %dx%d' % (f[2] - f[0], f[3] - f[1]))
print()

pad = REACH + 8
x0, y0 = f[0] - pad, f[1] - pad
w, h = (f[2] - f[0]) + pad * 2, (f[3] - f[1]) + pad * 2

scr = u.GetDC(0)
md = g.CreateCompatibleDC(scr)
bm = g.CreateCompatibleBitmap(scr, w, h)
g.SelectObject(md, bm)
g.BitBlt(md, 0, 0, w, h, scr, x0, y0, SRCCOPY)
info = BITMAPINFO()
info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
info.bmiHeader.biWidth = w
info.bmiHeader.biHeight = -h
info.bmiHeader.biPlanes = 1
info.bmiHeader.biBitCount = 32
buf = ctypes.create_string_buffer(w * h * 4)
g.GetDIBits(md, bm, 0, h, buf, ctypes.byref(info), 0)
g.DeleteObject(bm)
g.DeleteDC(md)
u.ReleaseDC(0, scr)


def at(x, y):
    ix, iy = x - x0, y - y0
    if ix < 0 or iy < 0 or ix >= w or iy >= h:
        return None
    off = (iy * w + ix) * 4
    return (buf[off + 2][0], buf[off + 1][0], buf[off][0])


def edge_depth(sx, sy, dx, dy):
    """从窗口外 REACH 处往里扫，返回可见边缘在窗口边界里面多少像素。

    阴影是渐变，实体边缘是突变，所以取相邻像素差最大的那一步。"""
    samples = []
    for s in range(-REACH, REACH + 1):
        p = at(sx + dx * s, sy + dy * s)
        samples.append(p)
    best_step = None
    best_diff = 0
    for i in range(1, len(samples)):
        a, b = samples[i - 1], samples[i]
        if a is None or b is None:
            continue
        diff = max(abs(a[j] - b[j]) for j in range(3))
        if diff > best_diff:
            best_diff = diff
            best_step = i - REACH - 1
    if best_step is None or best_diff < 24:
        return None
    return best_step


def median(values):
    vals = sorted(v for v in values if v is not None)
    if not vals:
        return None
    return vals[len(vals) // 2]


results = {}
for side, dx, dy, gen in (
        ('左', 1, 0, lambda t: (f[0], f[1] + int((f[3] - f[1]) * t))),
        ('右', -1, 0, lambda t: (f[2] - 1, f[1] + int((f[3] - f[1]) * t))),
        ('上', 0, 1, lambda t: (f[0] + int((f[2] - f[0]) * t), f[1])),
        ('下', 0, -1, lambda t: (f[0] + int((f[2] - f[0]) * t), f[3] - 1))):
    depths = []
    for t in [x / 20.0 for x in range(4, 17)]:
        sx, sy = gen(t)
        depths.append(edge_depth(sx, sy, dx, dy))
    got = median(depths)
    results[side] = got
    shown = '量不出来' if got is None else ('%d px' % got)
    hits = sum(1 for v in depths if v is not None)
    print('  %s边：可见边缘在窗口边界里面 %s（%d/%d 个采样有结果）'
          % (side, shown, hits, len(depths)))

print()
left = results['左'] or 0
top = results['上'] or 0
right = results['右'] or 0
bottom = results['下'] or 0
if left <= 0 and top <= 0 and right <= 0 and bottom <= 0:
    print('这个窗口的矩形和它看得见的部分基本一致，不需要配。')
else:
    print('把这一行加进 settings.conf 的 tracking.shadow_insets：')
    print('（多条之间用竖线 | 分隔，不是逗号——逗号在一条内部已经用掉了）')
    print()
    print('    %s:%d,%d,%d,%d' % (name, max(0, left), max(0, top),
                                  max(0, right), max(0, bottom)))
    print()
    print('顺序是 左,上,右,下。')
    print('这个值同时管两件事：给这个窗口画边框时贴着它的可见边缘，以及它挡住别人时')
    print('只算看得见的那一块——后者正是「别人的边框在它旁边断开过宽」的成因。')

    if '--apply' in sys.argv:
        entry = '%s:%d,%d,%d,%d' % (name, max(0, left), max(0, top),
                                    max(0, right), max(0, bottom))
        print()
        print('--apply：直接写进配置并重启。')
        import os
        import subprocess
        import time
        conf = os.path.join(os.environ['LOCALAPPDATA'], 'WindowMark', 'settings.conf')
        exe = os.path.join(os.environ['LOCALAPPDATA'], 'Programs', 'WindowMark',
                           'WindowMark.exe')
        key = 'tracking.shadow_insets='
        # 值是 URL 编码的，条目之间用竖线分隔——逗号在一条内部已经用掉了。
        encoded = entry.replace('%', '%25').replace(':', '%3A').replace(',', '%2C')
        with io.open(conf, encoding='utf-8') as fh:
            lines = fh.readlines()
        current = ''
        for line in lines:
            if line.startswith(key):
                current = line.strip()[len(key):]
                break
        prefix = encoded.split('%3A')[0] + '%3A'
        parts = [p for p in current.split('|') if p and not p.startswith(prefix)]
        parts.append(encoded)
        merged = '|'.join(parts)

        # 先杀进程再改文件：它退出时会把内存里的设置写回，顺序反了改动会被盖掉。
        subprocess.run(['taskkill', '/IM', 'WindowMark.exe', '/F'], capture_output=True)
        time.sleep(1.5)
        out = []
        seen = False
        for line in lines:
            if line.startswith(key):
                out.append(key + merged + '\n')
                seen = True
            else:
                out.append(line)
        if not seen:
            out.append(key + merged + '\n')
        with io.open(conf, 'w', encoding='utf-8', newline='') as fh:
            fh.writelines(out)
        subprocess.Popen([exe], close_fds=True)
        print('写好了，WindowMark 已重启。当前 shadow_insets：')
        print('  %s' % merged)
    else:
        print()
        print('（加 --apply 可以直接写进配置并重启，不用手抄）')
