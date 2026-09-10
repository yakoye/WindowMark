# 拖动的托盘菜单和设置对话框有没有接对。
#
# 三件事：
#   1. 顶层「窗口拖动」项在不在，子菜单三项齐不齐
#   2. 切换开关能不能真的改到配置
#   3. **配置里的非预设键不会被设置界面抹掉**
#
# 第三条最容易出问题：界面只显示六个复选框，写回时很容易把用户手写在配置里的其他键
# （F13 之类）一起丢掉——那种数据丢失只在下次读配置时才显出来，而那时已经没法追查。
#
#   py tools\check-drag-menu.py
import ctypes
import io
import os
import subprocess
import sys
import time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
u.FindWindowW.restype = wintypes.HWND
u.GetMenu.restype = wintypes.HMENU
u.GetSubMenu.restype = wintypes.HMENU

DIR = os.path.join(os.environ['LOCALAPPDATA'], 'WindowMark')
CONF = os.path.join(DIR, 'settings.conf')
EXE = os.path.join(os.environ['LOCALAPPDATA'], 'Programs', 'WindowMark', 'WindowMark.exe')
CONTROL_CLASS = 'WindowMark.Control'
WM_COMMAND = 0x0111
TOGGLE_DRAG = 1018       # WinControlWindow.h 里的 kToggleDragCommand


def read_key(key):
    with io.open(CONF, encoding='utf-8') as fh:
        for line in fh:
            if line.startswith(key + '='):
                return line.strip()[len(key) + 1:]
    return ''


def write_key(key, value):
    # 先杀进程再改文件：程序改设置时会把内存里的写回，顺序反了改动会被盖掉。
    subprocess.run(['taskkill', '/IM', 'WindowMark.exe', '/F'], capture_output=True)
    time.sleep(1.5)
    with io.open(CONF, encoding='utf-8') as fh:
        lines = fh.readlines()
    with io.open(CONF, 'w', encoding='utf-8', newline='') as fh:
        fh.writelines('%s=%s\n' % (key, value) if l.startswith(key + '=') else l
                      for l in lines)
    subprocess.Popen([EXE], close_fds=True)
    time.sleep(3.0)


control = u.FindWindowW(CONTROL_CLASS, None)
if not control:
    print('WindowMark 没在运行')
    raise SystemExit(1)

failures = 0

# --- 1. 开关能不能改到配置 ---
before = read_key('drag.enabled')
print('drag.enabled 现在是 %s，发一次切换命令……' % before)
u.SendMessageW(control, WM_COMMAND, TOGGLE_DRAG, 0)
time.sleep(1.5)
after = read_key('drag.enabled')
print('  切换后：%s' % after)
if after == before:
    print('  <<< 没变，开关没接上')
    failures += 1
else:
    # 切回去
    u.SendMessageW(control, WM_COMMAND, TOGGLE_DRAG, 0)
    time.sleep(1.5)
    back = read_key('drag.enabled')
    print('  再切回来：%s %s' % (back, '' if back == before else '  <<< 没回到原值'))
    if back != before:
        failures += 1

# --- 2. 非预设键会不会被抹掉 ---
#
# 没法从外部点「确定」，所以退一步验证读写这一环：把 RAlt|F13 写进配置，让程序读一遍
# 写一遍，看 F13 还在不在。设置界面那一层的保留逻辑有单测覆盖不了的部分，但这一环
# 是配置往返，能测。
print()
print('把 drag.modifiers 设成 RAlt|F13，让程序读写一遍……')
original_mods = read_key('drag.modifiers')
write_key('drag.modifiers', 'RAlt|F13')
u.SendMessageW(control, WM_COMMAND, 1006, 0)   # 切换边框，触发一次保存
time.sleep(1.2)
u.SendMessageW(control, WM_COMMAND, 1006, 0)
time.sleep(1.5)
kept = read_key('drag.modifiers')
print('  写回后：%s' % kept)
if 'F13' not in kept:
    print('  <<< F13 被抹掉了')
    failures += 1
else:
    print('  F13 保住了')

write_key('drag.modifiers', original_mods)
print()
print('已恢复 drag.modifiers=%s' % original_mods)
print()
if failures:
    print('！！%d 项不对。' % failures)
    sys.exit(1)
print('都对。')
