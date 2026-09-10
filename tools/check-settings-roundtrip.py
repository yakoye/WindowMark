# 配置文件读一遍写一遍，比对前后有没有字段丢失或变值。
#
# 读写不对称是这类配置代码最典型的 bug：加了新字段却只写了解析、或者只写了序列化，
# 用户改一次设置就丢掉一批东西。项目历史上「树控件复选框」那次数据丢失就是这样。
#
# 判据是**键值对**，不是整个文件的文本：注释和空行的排布变了没关系，键的集合和每个键
# 的值不能变。
#
#   py tools\check-settings-roundtrip.py
import io
import os
import subprocess
import sys
import tempfile

CONF = os.path.join(os.environ['LOCALAPPDATA'], 'WindowMark', 'settings.conf')


def read_pairs(path):
    pairs = {}
    with io.open(path, encoding='utf-8') as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if '=' not in line:
                continue
            key, value = line.split('=', 1)
            pairs[key.strip()] = value.strip()
    return pairs


if not os.path.isfile(CONF):
    print('找不到 %s' % CONF)
    raise SystemExit(1)

before = read_pairs(CONF)
print('当前配置有 %d 个键' % len(before))

# 让程序自己读一遍写一遍：退出时它会把内存里的设置写回去。
# 比自己解析可靠——要验的正是程序那一套读写，不是另写一份。
print('重启 WindowMark，让它读一遍写一遍……')
subprocess.run(['taskkill', '/IM', 'WindowMark.exe', '/F'], capture_output=True)
import time
time.sleep(1.5)
exe = os.path.join(os.environ['LOCALAPPDATA'], 'Programs', 'WindowMark', 'WindowMark.exe')
subprocess.Popen([exe], close_fds=True)
time.sleep(3.0)
subprocess.run(['taskkill', '/IM', 'WindowMark.exe', '/F'], capture_output=True)
time.sleep(1.5)
subprocess.Popen([exe], close_fds=True)
time.sleep(2.0)

after = read_pairs(CONF)
print('写回后有 %d 个键' % len(after))
print()

lost = sorted(set(before) - set(after))
added = sorted(set(after) - set(before))
changed = sorted(k for k in before if k in after and before[k] != after[k])

if lost:
    print('丢失的键（%d 个）：' % len(lost))
    for k in lost:
        print('  %s = %s' % (k, before[k]))
if added:
    print('新增的键（%d 个，通常是这一版加的新配置，正常）：' % len(added))
    for k in added:
        print('  %s = %s' % (k, after[k]))
if changed:
    print('值变了的键（%d 个）：' % len(changed))
    for k in changed:
        print('  %s：%s  ->  %s' % (k, before[k], after[k]))

if not lost and not changed:
    print('没有丢失，也没有值被改动。')
else:
    print()
    print('！！读写不对称。丢失的键说明只写了序列化没写解析（或反过来）。')
    sys.exit(1)
