# 键盘钩子只在配置勾了 Win 键时才装。
#
# 这条要验，因为它是个「不做什么」的承诺：只用 Alt / Ctrl 的用户，全程只该有一个鼠标
# 钩子。键盘钩子比鼠标钩子更敏感——杀毒软件更关注、出错影响更大——多装一个是实打实的
# 代价，而「没装」这件事从外面看不出来，只能靠日志。
#
# 做法：分别用两种配置启动，读日志里的钩子安装记录。
#
#   py tools\check-drag-hooks.py
import io
import os
import subprocess
import sys
import time

DIR = os.path.join(os.environ['LOCALAPPDATA'], 'WindowMark')
CONF = os.path.join(DIR, 'settings.conf')
FLAG = os.path.join(DIR, 'diag.on')
LOG = os.path.join(DIR, 'diag.log')
EXE = os.path.join(os.environ['LOCALAPPDATA'], 'Programs', 'WindowMark', 'WindowMark.exe')


def read_key(key):
    with io.open(CONF, encoding='utf-8') as fh:
        for line in fh:
            if line.startswith(key + '='):
                return line.strip()[len(key) + 1:]
    return ''


def write_keys(pairs):
    # 先杀进程再改文件：程序改设置时会把内存里的写回，顺序反了改动会被盖掉。
    subprocess.run(['taskkill', '/IM', 'WindowMark.exe', '/F'], capture_output=True)
    time.sleep(1.5)
    with io.open(CONF, encoding='utf-8') as fh:
        lines = fh.readlines()
    out = []
    for line in lines:
        replaced = False
        for key, value in pairs.items():
            if line.startswith(key + '='):
                out.append('%s=%s\n' % (key, value))
                replaced = True
                break
        if not replaced:
            out.append(line)
    with io.open(CONF, 'w', encoding='utf-8', newline='') as fh:
        fh.writelines(out)
    subprocess.Popen([EXE], close_fds=True)
    time.sleep(3.0)


def hook_lines():
    if not os.path.isfile(LOG):
        return []
    with io.open(LOG, encoding='utf-8-sig') as fh:
        return [l.rstrip() for l in fh if '钩子' in l]


original = {'drag.enabled': read_key('drag.enabled'),
            'drag.modifiers': read_key('drag.modifiers')}
print('原配置：%s' % original)

with io.open(FLAG, 'w') as fh:
    fh.write('on')

cases = [
    ('只有 RAlt', {'drag.enabled': 'true', 'drag.modifiers': 'RAlt'}, False),
    ('含 LWin', {'drag.enabled': 'true', 'drag.modifiers': 'RAlt|LWin'}, True),
    ('功能关闭', {'drag.enabled': 'false', 'drag.modifiers': 'RAlt|LWin'}, False),
]

failures = 0
for name, config, wantKeyboard in cases:
    if os.path.isfile(LOG):
        os.remove(LOG)
    write_keys(config)
    lines = hook_lines()
    mouse = any('鼠标钩子已装' in l for l in lines)
    keyboard = any('键盘钩子已装' in l for l in lines)
    wantMouse = config['drag.enabled'] == 'true'

    ok = (mouse == wantMouse) and (keyboard == wantKeyboard)
    if not ok:
        failures += 1
    print()
    print('%s（%s）：' % (name, config['drag.modifiers']))
    print('  鼠标钩子 %s（该 %s）' % ('装了' if mouse else '没装',
                                     '装' if wantMouse else '不装'))
    print('  键盘钩子 %s（该 %s）  %s'
          % ('装了' if keyboard else '没装', '装' if wantKeyboard else '不装',
             '' if ok else '  <<< 不对'))
    for l in lines:
        print('    %s' % l)

try:
    os.remove(FLAG)
except OSError:
    pass
write_keys(original)
print()
print('已恢复原配置。')
print()
if failures:
    print('！！%d 个用例不符合预期。' % failures)
    sys.exit(1)
print('都对：键盘钩子只在配置含 Win 键时才装。')
