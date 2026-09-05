# WindowMark 边框全量追踪。
#
# 一次把判断边框对错所需的全部事实打出来，不做推断：
#   显示器  监视器/工作区/DPI
#   窗口    进程、类名、HWND、窗口矩形、DWM 可见边界、z 序、是否前台、topmost、最大化
#   边框    配对到的边框窗口、它的矩形与 z 序、相对目标的位置
#   期望    按 ClampBorderToScreen 的规则独立算一遍边框该在哪
#   像素    截屏采样四条边，看每条边实际画出来了几个采样点
#   夹层    边框与目标之间夹着哪些可见窗口（会遮住边框，或被边框遮住）
#
# 用法：
#   python tools/trace-borders.py            当场采一次
#   python tools/trace-borders.py 8           采 8 秒，每次前台变化时各采一次
#
# 之所以要像素那一列：z 序全对而屏幕上就是看不见，这两件事同时成立过。只查 z 序会
# 得出「一切正常」的结论，而使用者看到的是缺一条边。
import ctypes
import sys
import time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
g = ctypes.WinDLL('gdi32', use_last_error=True)
d = ctypes.WinDLL('dwmapi', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
shcore = ctypes.WinDLL('shcore', use_last_error=True)
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

P = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
MonProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC,
                             ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)
SRCCOPY = 0x00CC0020
GW_HWNDPREV, GW_HWNDNEXT, GW_OWNER = 3, 2, 4
GWL_STYLE, GWL_EXSTYLE = -16, -20
WS_EX_TOPMOST = 0x00000008
BORDER_CLASS = 'WindowMark.WindowBorder'

# 边框颜色，来自 settings.conf 的 border.active_color / inactive_color 默认值。
ACTIVE = (0x62, 0x74, 0xE7)
INACTIVE = (0x70, 0x80, 0xAA)
COLOR_TOL = 44


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG),
                ('biHeight', wintypes.LONG), ('biPlanes', wintypes.WORD),
                ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD),
                ('biSizeImage', wintypes.DWORD), ('biXPelsPerMeter', wintypes.LONG),
                ('biYPelsPerMeter', wintypes.LONG), ('biClrUsed', wintypes.DWORD),
                ('biClrImportant', wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [('bmiHeader', BITMAPINFOHEADER), ('bmiColors', wintypes.DWORD * 3)]


class MONITORINFOEX(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.DWORD), ('rcMonitor', wintypes.RECT),
                ('rcWork', wintypes.RECT), ('dwFlags', wintypes.DWORD),
                ('szDevice', wintypes.WCHAR * 32)]


def cls(h):
    b = ctypes.create_unicode_buffer(160)
    u.GetClassNameW(h, b, 160)
    return b.value


def title(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetWindowTextW(h, b, 256)
    return b.value


def pname(h):
    pid = wintypes.DWORD()
    u.GetWindowThreadProcessId(h, ctypes.byref(pid))
    hp = k.OpenProcess(0x1000, False, pid)
    if not hp:
        return '?(高完整性)'
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


def screens_of(h):
    mi = MONITORINFOEX()
    mi.cbSize = ctypes.sizeof(mi)
    u.GetMonitorInfoW(u.MonitorFromWindow(h, 2), ctypes.byref(mi))
    m, w = mi.rcMonitor, mi.rcWork
    return ((m.left, m.top, m.right, m.bottom), (w.left, w.top, w.right, w.bottom))


def monitors():
    out = []

    def cb(hmon, hdc, lprc, lparam):
        mi = MONITORINFOEX()
        mi.cbSize = ctypes.sizeof(mi)
        u.GetMonitorInfoW(hmon, ctypes.byref(mi))
        x, y = wintypes.UINT(), wintypes.UINT()
        try:
            shcore.GetDpiForMonitor(hmon, 0, ctypes.byref(x), ctypes.byref(y))
        except Exception:
            x.value = 96
        m, w = mi.rcMonitor, mi.rcWork
        out.append((mi.szDevice, bool(mi.dwFlags & 1),
                    (m.left, m.top, m.right, m.bottom),
                    (w.left, w.top, w.right, w.bottom), x.value))
        return True

    u.EnumDisplayMonitors(None, None, MonProc(cb), 0)
    return out


def zlist():
    out = []
    h = u.GetTopWindow(None)
    while h:
        out.append(h)
        h = u.GetWindow(h, GW_HWNDNEXT)
    return out


def grab(x, y, w, h):
    if w <= 0 or h <= 0:
        return None
    screen = u.GetDC(0)
    mem = g.CreateCompatibleDC(screen)
    bmp = g.CreateCompatibleBitmap(screen, w, h)
    g.SelectObject(mem, bmp)
    g.BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY)
    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    g.DeleteObject(bmp)
    g.DeleteDC(mem)
    u.ReleaseDC(0, screen)

    def px(ix, iy):
        if ix < 0 or iy < 0 or ix >= w or iy >= h:
            return None
        off = (iy * w + ix) * 4
        v = buf[off:off + 3]
        return (v[2], v[1], v[0])

    return px


def is_border_color(p):
    if p is None:
        return False
    for ref in (ACTIVE, INACTIVE):
        if all(abs(p[i] - ref[i]) <= COLOR_TOL for i in range(3)):
            return True
    return False


def touches(gap, reach):
    return 0 <= gap <= reach


def expected_border(frame, mon, work, reach):
    """与 src/core/BorderGeometry.cpp 的 ClampBorderToScreen 对应的独立实现。"""
    o = [frame[0] - reach, frame[1] - reach, frame[2] + reach, frame[3] + reach]
    if reach <= 0:
        return tuple(o)
    if touches(frame[0] - work[0], reach):
        o[0] = max(o[0], work[0])
    elif touches(frame[0] - mon[0], reach):
        o[0] = max(o[0], mon[0])
    if touches(frame[1] - work[1], reach):
        o[1] = max(o[1], work[1])
    elif touches(frame[1] - mon[1], reach):
        o[1] = max(o[1], mon[1])
    if touches(work[2] - frame[2], reach):
        o[2] = min(o[2], work[2])
    elif touches(mon[2] - frame[2], reach):
        o[2] = min(o[2], mon[2])
    if touches(work[3] - frame[3], reach):
        o[3] = min(o[3], work[3])
    elif touches(mon[3] - frame[3], reach):
        o[3] = min(o[3], mon[3])
    return tuple(o)


def read_reach():
    """从 settings.conf 读 border.width + border.offset。"""
    import os
    path = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'WindowMark', 'settings.conf')
    width, offset = 4, -1
    try:
        with open(path, encoding='utf-8', errors='replace') as fh:
            for ln in fh:
                if ln.startswith('border.width='):
                    width = int(ln.split('=', 1)[1].strip())
                elif ln.startswith('border.offset='):
                    offset = int(ln.split('=', 1)[1].strip())
    except Exception:
        pass
    return max(0, width + offset)


def overlaps(a, b):
    return not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])


def topmost_at(point, order, idx):
    """在这个点上，z 序里最靠前的那个可见窗口是谁。

    纯几何判断，不用 WindowFromPoint——边框窗口是 layered + WS_DISABLED，命中测试
    未必认它，而这里要问的恰恰是「屏幕上这一点归谁画」。
    """
    x, y = point
    for h in order:
        if not u.IsWindowVisible(h) or u.IsIconic(h):
            continue
        r = rc(h)
        if (r[2] - r[0]) <= 0 or (r[3] - r[1]) <= 0:
            continue
        if r[0] <= x < r[2] and r[1] <= y < r[3]:
            return h, idx.get(h, -1)
    return None, -1


def edge_detail(frame, border, order, idx):
    """四条边逐条检查：像素画出来没，以及那条边所在位置最上面的窗口是谁（z 序多少）。

    像素和 z 序要一起看：像素告诉你「看不看得见」，压在上面的窗口告诉你「为什么」。
    """
    ring = 2
    fracs = (0.15, 0.35, 0.5, 0.65, 0.85)
    edges = {
        '上': [(frame[0] + int((frame[2] - frame[0]) * t), frame[1] - ring) for t in fracs],
        '下': [(frame[0] + int((frame[2] - frame[0]) * t), frame[3] + ring - 1) for t in fracs],
        '左': [(frame[0] - ring, frame[1] + int((frame[3] - frame[1]) * t)) for t in fracs],
        '右': [(frame[2] + ring - 1, frame[1] + int((frame[3] - frame[1]) * t)) for t in fracs],
    }
    pts = [p for v in edges.values() for p in v]
    x0, y0 = min(p[0] for p in pts) - 2, min(p[1] for p in pts) - 2
    x1, y1 = max(p[0] for p in pts) + 3, max(p[1] for p in pts) + 3
    px = grab(x0, y0, x1 - x0, y1 - y0)

    out = {}
    for name, ps in edges.items():
        hits = 0
        coverers = {}
        for (sx, sy) in ps:
            if px is not None and is_border_color(px(sx - x0, sy - y0)):
                hits += 1
            top, tz = topmost_at((sx, sy), order, idx)
            if top is None:
                key = '(空)'
            elif top == border:
                key = '边框自己 z=%d' % tz
            else:
                key = '%s(%s) z=%d' % (pname(top), cls(top)[:16], tz)
            coverers[key] = coverers.get(key, 0) + 1
        out[name] = (hits, len(ps), coverers)
    return out


def trace(check_pixels=True):
    reach = read_reach()
    order = zlist()
    idx = {h: i for i, h in enumerate(order)}
    fg = u.GetForegroundWindow()

    print('=== 显示器 ===')
    for dev, primary, mon, work, dpi in monitors():
        print('  %-14s %s 监视器%-26s 工作区%-26s DPI %d (%d%%)'
              % (dev, '主' if primary else '副', str(mon), str(work), dpi,
                 round(dpi / 96 * 100)))
    print('  边框外扩 reach = %d px（border.width + border.offset）' % reach)
    print('  前台窗口：%s (%s)' % (pname(fg), cls(fg)))
    print()

    borders = [h for h in order if u.IsWindowVisible(h) and cls(h) == BORDER_CLASS]
    print('=== 边框窗口共 %d 个 ===' % len(borders))
    print()

    used = set()
    problems = []
    for h in order:
        if not u.IsWindowVisible(h) or u.IsIconic(h):
            continue
        c = cls(h)
        if c.startswith('WindowMark.') or c == 'ClipKeeperMainWindow':
            continue
        f = dwm(h) or rc(h)
        if (f[2] - f[0]) < 250 or (f[3] - f[1]) < 150:
            continue

        cx, cy = (f[0] + f[2]) / 2, (f[1] + f[3]) / 2
        mine, bestd = None, 10 ** 9
        for b in borders:
            if b in used:
                continue
            r = rc(b)
            dist = abs((r[0] + r[2]) / 2 - cx) + abs((r[1] + r[3]) / 2 - cy)
            if dist < bestd:
                bestd, mine = dist, b
        if mine is None or bestd > 120:
            continue
        used.add(mine)

        w = rc(h)
        mon, work = screens_of(h)
        st = u.GetWindowLongPtrW(h, GWL_STYLE) & 0xFFFFFFFF
        ex = u.GetWindowLongPtrW(h, GWL_EXSTYLE) & 0xFFFFFFFF
        bex = u.GetWindowLongPtrW(mine, GWL_EXSTYLE) & 0xFFFFFFFF
        br = rc(mine)
        want = expected_border(f, mon, work, reach)
        zt, zb = idx.get(h, -1), idx.get(mine, -1)

        print('%s  [%s]  HWND 0x%X' % (pname(h), c, h))
        print('   窗口   z=%-4d 矩形%-26s DWM%-26s %s%s%s'
              % (zt, str(w), str(f),
                 '前台 ' if h == fg else '',
                 'topmost ' if ex & WS_EX_TOPMOST else '',
                 '最大化' if u.IsZoomed(h) else ''))
        print('   边框   z=%-4d 矩形%-26s 期望%-26s %s%s'
              % (zb, str(br), str(want),
                 '位置对' if tuple(br) == want else '位置错 <<<',
                 '  topmost' if bex & WS_EX_TOPMOST else ''))
        if tuple(br) != want:
            problems.append('%s 边框位置错' % pname(h))

        # z 序关系。判据跟着规则走：
        #   前台窗口 -> 边框该在最顶端（topmost 且前面没有可见窗口）
        #   其他窗口 -> 边框该贴在自己目标的下方，被前面的窗口盖住是正常的
        if zb < zt:
            rel = '边框在目标前面 %d 位' % (zt - zb)
        elif zb > zt:
            rel = '边框在目标后面 %d 位' % (zb - zt)
        else:
            rel = '同位'
        if h == fg:
            blocker = None
            probe = mine
            for _ in range(4096):
                probe = u.GetWindow(probe, GW_HWNDPREV)
                if not probe:
                    break
                if not u.IsWindowVisible(probe) or u.IsIconic(probe):
                    continue
                blocker = probe
                break
            if not (bex & WS_EX_TOPMOST):
                rel += '  <<< 前台窗口的边框却不是 topmost'
                problems.append('%s 是前台，边框却不在 topmost 层' % pname(h))
            elif blocker is not None:
                # 不算问题：前台边框只保证在 topmost 层，不抢最前，好让右键菜单、
                # 输入法候选框、任务栏能压在它上面。这里打出来纯粹是给人看的。
                rel += ('  （上面压着 %s(z=%d)，若是菜单/候选框/任务栏则属正常）'
                        % (pname(blocker), idx.get(blocker, -1)))
        elif zb < zt:
            rel += '  （非前台窗口的边框本该在目标下方）'
        lo, hi = min(zt, zb), max(zt, zb)
        sandwich = []
        for other in order[lo + 1:hi]:
            if other in (h, mine) or not u.IsWindowVisible(other) or u.IsIconic(other):
                continue
            orc = rc(other)
            if (orc[2] - orc[0]) < 60 or (orc[3] - orc[1]) < 60:
                continue
            if overlaps(orc, br):
                sandwich.append('%s(%s,z=%d)' % (pname(other), cls(other)[:18],
                                                 idx.get(other, -1)))
        print('   z序    %s' % rel)
        if sandwich:
            mark = '  <<<' if h == fg else ''
            print('          中间夹着：%s%s' % ('  '.join(sandwich[:4]), mark))
            if h == fg:
                problems.append('%s 是前台，边框与目标之间却夹着 %d 个可见窗口'
                                % (pname(h), len(sandwich)))

        if check_pixels:
            eg = edge_detail(f, mine, order, idx)
            desc = '  '.join('%s%d/%d' % (name, hit, tot)
                             for name, (hit, tot, _) in eg.items())
            missing = [name for name, (hit, tot, _) in eg.items() if hit == 0]
            partial = [name for name, (hit, tot, _) in eg.items() if 0 < hit < tot]
            note = ''
            if missing:
                note = '  <<< %s 边完全没画出来' % ' '.join(missing)
                if h == fg:
                    problems.append('%s 是前台，%s 边却看不见'
                                    % (pname(h), ' '.join(missing)))
            elif partial:
                note = '  （%s 边部分被遮）' % ' '.join(partial)
            print('   像素   %s%s' % (desc, note))
            # 只有没画全的边才需要解释「为什么」，画全了的边打出来是噪音。
            for name in missing + partial:
                hit, tot, coverers = eg[name]
                who = '  '.join('%s x%d' % (k, v) for k, v in
                                sorted(coverers.items(), key=lambda kv: -kv[1]))
                print('          %s边 %d/%d  这条线上最靠前的是：%s' % (name, hit, tot, who))
        print()

    print('=== 小结 ===')
    if problems:
        for p in problems:
            print('  ! %s' % p)
    else:
        print('  没有发现异常')
    return problems


def main():
    seconds = 0
    if len(sys.argv) > 1:
        try:
            seconds = int(sys.argv[1])
        except ValueError:
            seconds = 0

    if seconds <= 0:
        trace()
        return 0

    print('追踪 %d 秒：每次前台窗口变化后 0.4 秒各采一次。请正常点击切换窗口。' % seconds)
    print()
    last = None
    end = time.time() + seconds
    while time.time() < end:
        fg = u.GetForegroundWindow()
        if fg != last:
            last = fg
            time.sleep(0.4)
            print('#' * 100)
            print('# 前台切到 %s' % pname(fg))
            print('#' * 100)
            trace()
            print()
        time.sleep(0.05)
    return 0


if __name__ == '__main__':
    sys.exit(main())
