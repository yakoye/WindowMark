"""单 EXE 离屏验收：不启动托盘、不附加 Explorer，不改变正在运行的实例。"""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import time

root = Path(__file__).resolve().parents[1]
build = root / 'build/Release'
evidence = root / 'build/evidence' / ('single-exe-' + time.strftime('%Y%m%d-%H%M%S'))
app = evidence / '中文 空格目录'
app.mkdir(parents=True)
shutil.copy2(build/'WindowMark.exe', app/'WindowMark.exe')
env = os.environ.copy()
env['LOCALAPPDATA'] = str(evidence/'不应写入 AppData')
env['APPDATA'] = str(evidence/'不应写入 Roaming')
results = []
def check(condition, label):
    assert condition, label
    results.append('PASS '+label)
def run(folder, expected=0):
    result = subprocess.run([str(folder/'WindowMark.exe'), '--prepare-portable'], env=env, timeout=30)
    check(result.returncode == expected, '准备命令退出码 '+str(expected))
def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

check(list(app.iterdir()) == [app/'WindowMark.exe'], '首次只有一个 EXE')
run(app)
check((app/'settings.conf').is_file(), '同目录创建配置')
check(not Path(env['LOCALAPPDATA']).exists() and not Path(env['APPDATA']).exists(), '不写 AppData')
cache = next((app/'.runtime').iterdir())
names = ['ClipKeeper.exe', 'WindowMarkDiag.exe', 'WindowMarkInspect.exe',
         'WindowMarkXamlContainerTrial.exe', 'WindowMarkXamlContainerTap.dll']
check(all(digest(cache/n) == digest(build/n) for n in names), '五个内置组件内容一致')
before = {n: (cache/n).stat().st_mtime_ns for n in names}
run(app)
check(all((cache/n).stat().st_mtime_ns == before[n] for n in names), '重复准备不重写完整缓存')
(cache/names[0]).write_bytes(b'broken cache')
(cache/names[1]).unlink()
run(app)
check(all(digest(cache/n) == digest(build/n) for n in names), '缺失和损坏组件自动修复')
conf = app/'settings.conf'
text = conf.read_text(encoding='utf-8')
assert 'ime_guard.show_hints=true' in text
conf.write_text(text.replace('ime_guard.show_hints=true', 'ime_guard.show_hints=false'), encoding='utf-8')
moved = evidence/'搬迁后'
app.rename(moved)
run(moved)
check('ime_guard.show_hints=false' in (moved/'settings.conf').read_text(encoding='utf-8'), '搬迁后保留用户设置')
blocked = evidence/'缓存目录不可创建'
blocked.mkdir()
shutil.copy2(build/'WindowMark.exe', blocked/'WindowMark.exe')
(blocked/'.runtime').write_text('阻止创建缓存目录',encoding='utf-8')
run(blocked, 7)
check(not (blocked/'settings.conf').exists(), '准备失败不伪装为成功')
check(not Path(env['LOCALAPPDATA']).exists() and not Path(env['APPDATA']).exists(), '失败也不改写 AppData')
(evidence/'result.md').write_text('# 单 EXE 离屏验收\n\n'+'\n'.join(results),encoding='utf-8')
print('\n'.join(results))
print(evidence)
