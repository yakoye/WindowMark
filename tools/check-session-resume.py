# 锁屏挂起之后，非 UNLOCK 的会话事件能不能把边框恢复回来。
#
# 真实场景：机器锁过一次，之后走 ToDesk 之类的远程接入，到达的是 CONSOLE_CONNECT /
# REMOTE_CONNECT，UNLOCK 未必来。原来的代码只认 UNLOCK，于是永远停在挂起——进程好好
# 跑着、消息循环正常、一条边框也不画，画布尺寸还停在显示配置变化之前（实测挂起时画布
# 还是 1536x864，屏幕已经换成 1920x1080）。
#
# 观察的是诊断日志里的状态机，不是屏幕像素。像素靠不住：锁屏界面或任何全屏窗口盖在
# 上面时，边框本来就该一个不画，那时「屏幕上没有边框」根本区分不出挂起和被遮挡。
#
# 直接给它的 message-only 窗口发 WM_WTSSESSION_CHANGE，不用真去锁屏。
#
#   py tools\check-session-resume.py
import io
import os
import ctypes
import sys
import time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
u.FindWindowExW.restype = wintypes.HWND
u.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR,
                            wintypes.LPCWSTR]

WM_WTSSESSION_CHANGE = 0x02B1
HWND_MESSAGE = wintypes.HWND(-3)

DIR = os.path.join(os.environ['LOCALAPPDATA'], 'WindowMark')
DIAG_ON = os.path.join(DIR, 'diag.on')
DIAG_LOG = os.path.join(DIR, 'diag.log')

# 只有 LOCK 该挂起，其余一律该恢复。
SUSPEND = [('WTS_SESSION_LOCK', 0x7)]
RESUME = [('WTS_CONSOLE_CONNECT', 0x1),
          ('WTS_REMOTE_CONNECT', 0x3),
          ('WTS_SESSION_LOGON', 0x5),
          ('WTS_SESSION_UNLOCK', 0x8),
          ('WTS_CONSOLE_DISCONNECT', 0x2),
          ('WTS_REMOTE_DISCONNECT', 0x4)]

session = u.FindWindowExW(HWND_MESSAGE, None, 'WindowMark.SessionWatch', None)
if not session:
    print('找不到 WindowMark.SessionWatch，程序没在跑？')
    sys.exit(1)
print('会话监听窗口 %s' % hex(int(session)))

had_diag = os.path.exists(DIAG_ON)
if not had_diag:
    io.open(DIAG_ON, 'w').close()
    time.sleep(0.3)


def log_tail():
    if not os.path.exists(DIAG_LOG):
        return []
    with io.open(DIAG_LOG, encoding='utf-8', errors='replace') as fh:
        return [l for l in fh if '会话' in l]


def send(code):
    mark = len(log_tail())
    u.SendMessageW(session, WM_WTSSESSION_CHANGE, code, 0)
    time.sleep(0.9)
    return log_tail()[mark:]


failures = []
print()
for name, code in RESUME:
    # 每轮都先锁一次，确保起点是挂起——否则「本来就在跑」会让结果没有意义。
    locked = send(0x7)
    if not any('挂起' in l for l in locked):
        failures.append('LOCK 没挂起（发 %s 之前）' % name)
        print('  LOCK 没挂起  <<<')
        continue
    lines = send(code)
    ok = any('恢复' in l for l in lines)
    print('  锁定后发 %-24s -> %s' % (name, '恢复' if ok else '仍然挂起  <<<'))
    if not ok:
        failures.append('%s 没能把挂起状态拉回来' % name)

# 反向：LOCK 必须真的挂起，不能被上面的改动带成「什么都恢复」
send(0x8)
lines = send(0x7)
locked_ok = any('挂起' in l for l in lines)
print('  LOCK 仍然挂起：%s' % ('是' if locked_ok else '否  <<< 锁屏时不该继续画'))
if not locked_ok:
    failures.append('LOCK 不再挂起了——锁屏界面上会留着边框')

# 收尾：恢复成正常状态
send(0x8)
if not had_diag:
    try:
        os.remove(DIAG_ON)
    except OSError:
        pass

print()
if failures:
    print('！！%d 项不对：' % len(failures))
    for f in failures:
        print('  - ' + f)
    sys.exit(1)
print('都对：只有 LOCK 挂起，其余会话事件都能把它拉回来。')
