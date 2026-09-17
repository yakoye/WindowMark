# 鼠标可穿透的全屏透明置顶窗口，会不会把所有边框都裁掉。
#
# 用户报告：装了 WGestures（鼠标手势）之后，屏幕上所有边框都没了；二分到 v0.4.8 正常、
# v0.4.9 起不正常。WGestures 常驻一个铺满全屏、置顶、鼠标可穿透的透明窗口画手势轨迹——
# 眼睛看不见，系统却说它「可见」。v0.4.9 起边框改成每屏一张画布、遮挡由 WindowMark 自己
# 用矩形算，这种窗口被当成一块不透明的板子，下面所有边框被整圈裁光。
#
# 这里不装 WGestures，自己造同样的窗口：
#   A  WS_EX_LAYERED | WS_EX_TRANSPARENT，内容全透明——覆盖层让鼠标穿透的标准做法
#   B  只有 WS_EX_LAYERED、像素 alpha 全是 0——靠透明像素让鼠标穿过去，样式位上看不出来
# 两种都是 NOACTIVATE + TOOLWINDOW，不抢焦点，不进任务栏，眼睛完全看不见。
#
# 判据是整块屏上边框颜色的像素数：挂上覆盖层前后比一比。直边是像素填充、颜色精确，
# 数精确匹配就够了，不用容差。
#
# 需要 WindowMark 正在运行、屏幕上有带边框的窗口（不能全是最大化的）。
#
#   py tools\check-transparent-overlay.py
import ctypes
import io
import os
import sys
import threading
import time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
g = ctypes.WinDLL('gdi32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
                             wintypes.LPARAM)
u.DefWindowProcW.restype = LRESULT
u.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u.CreateWindowExW.restype = wintypes.HWND
u.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                              wintypes.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                              ctypes.c_int, wintypes.HWND, wintypes.HMENU,
                              wintypes.HINSTANCE, wintypes.LPVOID]
# 句柄在 64 位上是 64 位的。只给返回值声明类型、不给参数声明的话，ctypes 会把句柄按
# 32 位 int 传进下一个函数，直接 OverflowError——所以用到的签名一个不落全写上。
def sig(fn, restype, *argtypes):
    fn.restype = restype
    fn.argtypes = list(argtypes)


sig(u.FindWindowW, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR)
sig(u.WindowFromPoint, wintypes.HWND, wintypes.POINT)
sig(u.GetAncestor, wintypes.HWND, wintypes.HWND, wintypes.UINT)
sig(u.GetDC, wintypes.HDC, wintypes.HWND)
sig(u.ReleaseDC, ctypes.c_int, wintypes.HWND, wintypes.HDC)
sig(u.ShowWindow, wintypes.BOOL, wintypes.HWND, ctypes.c_int)
sig(u.DestroyWindow, wintypes.BOOL, wintypes.HWND)
sig(u.RegisterClassExW, wintypes.ATOM, ctypes.c_void_p)
sig(u.PeekMessageW, wintypes.BOOL, ctypes.c_void_p, wintypes.HWND, wintypes.UINT,
    wintypes.UINT, wintypes.UINT)
sig(u.TranslateMessage, wintypes.BOOL, ctypes.c_void_p)
sig(u.DispatchMessageW, LRESULT, ctypes.c_void_p)
sig(u.UpdateLayeredWindow, wintypes.BOOL, wintypes.HWND, wintypes.HDC, ctypes.c_void_p,
    ctypes.c_void_p, wintypes.HDC, ctypes.c_void_p, wintypes.COLORREF, ctypes.c_void_p,
    wintypes.DWORD)
sig(g.CreateCompatibleDC, wintypes.HDC, wintypes.HDC)
sig(g.CreateCompatibleBitmap, wintypes.HBITMAP, wintypes.HDC, ctypes.c_int, ctypes.c_int)
sig(g.CreateDIBSection, wintypes.HBITMAP, wintypes.HDC, ctypes.c_void_p, wintypes.UINT,
    ctypes.c_void_p, wintypes.HANDLE, wintypes.DWORD)
sig(g.SelectObject, wintypes.HGDIOBJ, wintypes.HDC, wintypes.HGDIOBJ)
sig(g.BitBlt, wintypes.BOOL, wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    ctypes.c_int, wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD)
sig(g.GetDIBits, ctypes.c_int, wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
    ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT)
sig(g.DeleteObject, wintypes.BOOL, wintypes.HGDIOBJ)
sig(g.DeleteDC, wintypes.BOOL, wintypes.HDC)
sig(k.GetModuleHandleW, wintypes.HMODULE, wintypes.LPCWSTR)

WS_POPUP = 0x80000000
WS_EX_TOPMOST = 0x00000008
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_LAYERED = 0x00080000
WS_EX_NOACTIVATE = 0x08000000
SW_SHOWNOACTIVATE = 4
ULW_ALPHA = 0x2
AC_SRC_OVER = 0x0
AC_SRC_ALPHA = 0x1
PM_REMOVE = 0x1
SRCCOPY = 0x00CC0020
CLASS_NAME = 'WindowMarkTestClickThroughOverlay'


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.UINT), ('style', wintypes.UINT),
                ('lpfnWndProc', WNDPROC), ('cbClsExtra', ctypes.c_int),
                ('cbWndExtra', ctypes.c_int), ('hInstance', wintypes.HINSTANCE),
                ('hIcon', wintypes.HICON), ('hCursor', wintypes.HANDLE),
                ('hbrBackground', wintypes.HBRUSH), ('lpszMenuName', wintypes.LPCWSTR),
                ('lpszClassName', wintypes.LPCWSTR), ('hIconSm', wintypes.HICON)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG),
                ('biHeight', wintypes.LONG), ('biPlanes', wintypes.WORD),
                ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD),
                ('biSizeImage', wintypes.DWORD), ('biXPelsPerMeter', wintypes.LONG),
                ('biYPelsPerMeter', wintypes.LONG), ('biClrUsed', wintypes.DWORD),
                ('biClrImportant', wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [('bmiHeader', BITMAPINFOHEADER), ('bmiColors', wintypes.DWORD * 3)]


class BLENDFUNCTION(ctypes.Structure):
    _fields_ = [('BlendOp', ctypes.c_ubyte), ('BlendFlags', ctypes.c_ubyte),
                ('SourceConstantAlpha', ctypes.c_ubyte), ('AlphaFormat', ctypes.c_ubyte)]


class MSG(ctypes.Structure):
    _fields_ = [('hwnd', wintypes.HWND), ('message', wintypes.UINT),
                ('wParam', wintypes.WPARAM), ('lParam', wintypes.LPARAM),
                ('time', wintypes.DWORD), ('pt', wintypes.POINT)]


VX, VY = u.GetSystemMetrics(76), u.GetSystemMetrics(77)
VW, VH = u.GetSystemMetrics(78), u.GetSystemMetrics(79)


# ---- 边框颜色：配置里改过就用配置的 ----

def border_colors():
    active, inactive = '6274E7', '7080AA'
    conf = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'WindowMark', 'settings.conf')
    if os.path.exists(conf):
        with io.open(conf, encoding='utf-8', errors='replace') as fh:
            for line in fh:
                line = line.strip()
                if line.startswith('border.active_color=#'):
                    active = line.split('#', 1)[1][:6]
                elif line.startswith('border.inactive_color=#'):
                    inactive = line.split('#', 1)[1][:6]
    # 屏幕抓图是 BGRx，按字节序拼出要找的三个字节
    def bgr(hex6):
        r, gg, b = int(hex6[0:2], 16), int(hex6[2:4], 16), int(hex6[4:6], 16)
        return bytes((b, gg, r))
    return (active, bgr(active)), (inactive, bgr(inactive))


COLORS = border_colors()


def border_pixels():
    scr = u.GetDC(None)
    mem = g.CreateCompatibleDC(scr)
    bmp = g.CreateCompatibleBitmap(scr, VW, VH)
    old = g.SelectObject(mem, bmp)
    g.BitBlt(mem, 0, 0, VW, VH, scr, VX, VY, SRCCOPY)
    info = BITMAPINFO()
    info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.bmiHeader.biWidth = VW
    info.bmiHeader.biHeight = -VH
    info.bmiHeader.biPlanes = 1
    info.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(VW * VH * 4)
    g.GetDIBits(mem, bmp, 0, VH, buf, ctypes.byref(info), 0)
    g.SelectObject(mem, old)
    g.DeleteObject(bmp)
    g.DeleteDC(mem)
    u.ReleaseDC(None, scr)
    raw = buf.raw
    return sum(raw.count(pattern) for _, pattern in COLORS)


# ---- 覆盖层 ----

@WNDPROC
def wndproc(hwnd, msg, wparam, lparam):
    return u.DefWindowProcW(hwnd, msg, wparam, lparam)


class Overlay:
    """在自己的线程里建窗口、泵消息。窗口必须由泵消息的那个线程创建。"""

    def __init__(self, exstyle, block=False):
        self.exstyle = exstyle
        self.block = block
        self.hwnd = None
        self.ready = threading.Event()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self):
        self.thread.start()
        if not self.ready.wait(5):
            raise SystemExit('覆盖层 5 秒内没建出来')
        if not self.hwnd:
            raise SystemExit('CreateWindowExW 失败，错误 %d' % self.error)
        return self

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join(5)

    def _run(self):
        hinst = k.GetModuleHandleW(None)
        wc = WNDCLASSEXW()
        wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
        wc.lpfnWndProc = wndproc
        wc.hInstance = hinst
        wc.lpszClassName = CLASS_NAME
        u.RegisterClassExW(ctypes.byref(wc))   # 第二次注册会失败，类已经在，无所谓

        self.hwnd = u.CreateWindowExW(self.exstyle, CLASS_NAME, '', WS_POPUP,
                                      VX, VY, VW, VH, None, None, hinst, None)
        self.error = ctypes.get_last_error()
        if self.hwnd:
            self._paint_transparent()
            u.ShowWindow(self.hwnd, SW_SHOWNOACTIVATE)
        self.ready.set()

        msg = MSG()
        while not self.stop.is_set():
            while u.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
                u.TranslateMessage(ctypes.byref(msg))
                u.DispatchMessageW(ctypes.byref(msg))
            time.sleep(0.01)
        if self.hwnd:
            u.DestroyWindow(self.hwnd)

    def _paint_transparent(self):
        # 32 位 DIB 刚建出来全是 0，也就是逐像素 alpha 全为 0：整张完全透明。
        scr = u.GetDC(None)
        mem = g.CreateCompatibleDC(scr)
        info = BITMAPINFO()
        info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        info.bmiHeader.biWidth = VW
        info.bmiHeader.biHeight = -VH
        info.bmiHeader.biPlanes = 1
        info.bmiHeader.biBitCount = 32
        bits = ctypes.c_void_p()
        dib = g.CreateDIBSection(scr, ctypes.byref(info), 0, ctypes.byref(bits), None, 0)
        if self.block:
            # 对照组：正中一块 32x32 不透明灰色。WindowMark 的探测点有一个就在正中，
            # 打中它 → 这个窗口接得住鼠标 → 算遮挡物。预乘 alpha，BGRA 顺序。
            pixel = bytes((0x80, 0x80, 0x80, 0xFF))
            row = pixel * 32
            cx, cy = VW // 2, VH // 2
            for y in range(cy - 16, cy + 16):
                ctypes.memmove(bits.value + (y * VW + cx - 16) * 4, row, len(row))
        old = g.SelectObject(mem, dib)
        blend = BLENDFUNCTION(AC_SRC_OVER, 0, 255, AC_SRC_ALPHA)
        dst = wintypes.POINT(VX, VY)
        src = wintypes.POINT(0, 0)
        size = wintypes.SIZE(VW, VH)
        u.UpdateLayeredWindow(self.hwnd, scr, ctypes.byref(dst), ctypes.byref(size), mem,
                              ctypes.byref(src), 0, ctypes.byref(blend), ULW_ALPHA)
        g.SelectObject(mem, old)
        g.DeleteObject(dib)
        g.DeleteDC(mem)
        u.ReleaseDC(None, scr)


def settle(predicate, seconds=4.0):
    """等 WindowMark 反应过来：条件满足就提前返回，超时返回最后一次读数。"""
    deadline = time.time() + seconds
    value = border_pixels()
    while time.time() < deadline:
        if predicate(value):
            return value
        time.sleep(0.25)
        value = border_pixels()
    return value


def hit_through(hwnd):
    """屏幕中心的命中测试会不会穿过这个窗口。"""
    probe = wintypes.POINT(VX + VW // 2, VY + VH // 2)
    hit = u.WindowFromPoint(probe)
    root = u.GetAncestor(hit, 2) if hit else None
    return not root or int(root) != int(hwnd)


# ---- 主流程 ----

if not u.FindWindowW('WindowMark.Control', None):
    print('WindowMark 没在运行，测不了')
    sys.exit(1)

print('边框颜色：#%s（活动）  #%s（非活动）' % (COLORS[0][0], COLORS[1][0]))
# WindowMark 可能刚启动还没画出来，给它几秒
baseline = settle(lambda v: v >= 200, 8.0)
print('挂覆盖层之前：屏幕上 %d 个边框像素' % baseline)
if baseline < 200:
    print('！！边框像素太少，说明现在屏幕上几乎没有边框（窗口都最大化了？边框关着？），'
          '这条测试说明不了任何事')
    sys.exit(1)

# expect：keep = 边框应该保住，clip = 边框应该被裁掉
VARIANTS = [
    ('A', 'WS_EX_LAYERED | WS_EX_TRANSPARENT（标准的鼠标穿透覆盖层）',
     WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
     False, 'keep'),
    ('B', '只有 WS_EX_LAYERED，靠 alpha 为 0 的像素穿透',
     WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
     False, 'keep'),
    # 对照组。两个作用：证明修复没有矫枉过正（接得住鼠标的窗口照样算遮挡）；也证明
    # WindowMark 确实对测试窗口做出了反应——否则 A、B「边框保住了」可能只是它根本没看见。
    ('C', '对照组：同样全屏透明，但正中一块不透明、接得住鼠标',
     WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
     True, 'clip'),
]

results = {}
for key, label, exstyle, block, expect in VARIANTS:
    print()
    print('--- 覆盖层 %s：%s ---' % (key, label))
    with Overlay(exstyle, block) as overlay:
        through = hit_through(overlay.hwnd)
        print('  命中测试：屏幕中心%s这个窗口' % ('穿过了' if through else '打中了'))
        during = settle(lambda v: v < baseline * 0.1)
        print('  挂着的时候：%d 个边框像素（起步的 %d%%）'
              % (during, 100 * during // max(1, baseline)))
    after = settle(lambda v: v >= baseline * 0.8)
    print('  撤掉之后：%d 个边框像素' % after)
    results[key] = (during, after, through)

print()
print('=== 结论 ===')
failures = []
for key, label, _, _, expect in VARIANTS:
    during, after, through = results[key]
    ratio = during / max(1, baseline)
    if after < baseline * 0.5:
        verdict = '说明不了——撤掉覆盖层后边框也没回来，可能是测试期间窗口动过'
        failures.append(key + ' 说明不了')
    elif ratio >= 0.5:
        verdict = '边框保住了'
        if expect != 'keep':
            failures.append(key + ' 本该裁掉边框却没裁')
    elif ratio < 0.1:
        verdict = '边框被整片裁光'
        if expect != 'clip':
            failures.append(key + ' 把边框整片裁光了（就是用户报的问题）')
    else:
        verdict = '边框少了一大半，情况不明'
        failures.append(key + ' 情况不明')
    print('  %s  %s（期望：%s）' % (key, verdict, '保住' if expect == 'keep' else '裁掉'))

print()
if failures:
    print('！！%d 项不对：' % len(failures))
    for f in failures:
        print('  - ' + f)
    if any(f.startswith('C ') for f in failures):
        print('  对照组 C 没裁掉边框：WindowMark 可能根本没对测试窗口做出反应，A、B 的结果不可信。')
    sys.exit(1)
print('都对：两种鼠标穿透的覆盖层都不再裁边框，接得住鼠标的照样算遮挡。')
