# 让程序把配置读一遍写一遍，比对前后有没有字段丢失或变值。
#
# 读写不对称是这类配置代码最典型的 bug：加了新字段却只写了解析、或者只写了序列化，
# 用户改一次设置就丢掉一批东西。项目历史上「树控件复选框」那次数据丢失就是这样。
#
# **必须真的触发一次写。** 程序退出时不保存配置（persist 只在用户改设置时调用），所以
# 「重启一次再比对」什么也测不出来——文件根本没被碰过，比对必然通过。那种假的确认比
# 没有工具更糟。这里发两次「切换边框」的托盘命令：状态切出去再切回来，最终不变，但
# 每次都会走完整的保存路径。
#
# 判据是**键值对**，不是整个文件的文本：注释和空行的排布变了没关系，键的集合和每个键
# 的值不能变。
#
#   py tools\check-settings-roundtrip.py
import ctypes
import io
import os
import sys
import time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
u.FindWindowW.restype = wintypes.HWND

CONF = os.path.join(os.environ['LOCALAPPDATA'], 'WindowMark', 'settings.conf')
CONTROL_CLASS = 'WindowMark.Control'
WM_COMMAND = 0x0111
# WinControlWindow.h 里的 kToggleBordersCommand。选它是因为它切换的是一个纯布尔开关，
# 切两次必然回到原状态，不像「选择应用」那类会弹窗。
TOGGLE_BORDERS = 1006


def read_pairs(path):
    pairs = {}
    with io.open(path, encoding='utf-8') as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith('#') or '=' not in line:
                continue
            key, value = line.split('=', 1)
            pairs[key.strip()] = value.strip()
    return pairs


if not os.path.isfile(CONF):
    print('找不到 %s' % CONF)
    raise SystemExit(1)

control = u.FindWindowW(CONTROL_CLASS, None)
if not control:
    print('WindowMark 没在运行——先启动它')
    raise SystemExit(1)

before = read_pairs(CONF)
mtime_before = os.path.getmtime(CONF)
print('当前配置有 %d 个键' % len(before))

print('发两次「切换边框」，让它走完整的保存路径（状态切出去再切回来）……')
u.SendMessageW(control, WM_COMMAND, TOGGLE_BORDERS, 0)
time.sleep(1.0)
u.SendMessageW(control, WM_COMMAND, TOGGLE_BORDERS, 0)
time.sleep(1.5)

mtime_after = os.path.getmtime(CONF)
if mtime_after == mtime_before:
    print()
    print('！！配置文件没有被改写——这次比对说明不了任何问题。')
    print('   要么命令 ID 不对（看 WinControlWindow.h 里的 kToggleBordersCommand），')
    print('   要么保存路径没走到。')
    raise SystemExit(1)

after = read_pairs(CONF)
print('写回后有 %d 个键' % len(after))
print()

def same_content(a, b):
    """值是否等价。编码列表（竖线分隔）只比内容，不比顺序。

    EncodeList 会排序去重——那是有意的，配置文件不该因为勾选顺序不同而产生 diff。
    手工往配置里追加过条目的话，程序写回时会重排，那不是读写不对称。"""
    if a == b:
        return True
    if '|' in a or '|' in b:
        return sorted(x for x in a.split('|') if x) == sorted(x for x in b.split('|') if x)
    return False


lost = sorted(set(before) - set(after))
added = sorted(set(after) - set(before))
changed = sorted(k for k in before
                 if k in after and not same_content(before[k], after[k]))
reordered = sorted(k for k in before
                   if k in after and before[k] != after[k] and same_content(before[k], after[k]))

if lost:
    print('丢失的键（%d 个）：' % len(lost))
    for k in lost:
        print('  %s = %s' % (k, before[k]))
if added:
    print('新增的键（%d 个，这一版新加的配置第一次写出来，正常）：' % len(added))
    for k in added:
        print('  %s = %s' % (k, after[k]))
if reordered:
    print('顺序被规范化的键（%d 个，内容没变，正常）：' % len(reordered))
    for k in reordered:
        print('  %s' % k)
if changed:
    print('值变了的键（%d 个）：' % len(changed))
    for k in changed:
        print('  %s：%s  ->  %s' % (k, before[k], after[k]))

print()
if not lost and not changed:
    print('没有丢失，也没有值被改动。')
else:
    print('！！读写不对称。丢失的键说明只写了序列化没写解析（或反过来）；')
    print('   值变了说明写出的形式和读回来的解析对不上。')
    sys.exit(1)
