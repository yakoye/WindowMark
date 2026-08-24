"""确认 check-escapes.py 真能抓到当初那个 bug，并且不误报正常写法。"""
import importlib.util
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    'chk', os.path.join(here, 'check-escapes.py'))
chk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chk)

B = chk  # scan_literal

cases = [
    # (字面量, 应该报警吗, 说明)
    ('L"Software' + chr(92) + 'Microsoft' + chr(92) + 'Windows' + chr(92) + 'DWM"',
     True, '当初那个 bug：非法转义 \\M \\W \\D'),
    ('L"Software' + chr(92) * 2 + 'Microsoft' + chr(92) * 2 + 'Windows' + chr(92) * 2 + 'DWM"',
     False, '正确写法：双反斜杠'),
    ('"C:' + chr(92) + 'new' + chr(92) + 'file"',
     True, '/WX 抓不到的：合法转义 \\n 吃掉了路径分隔符'),
    ('"AppData' + chr(92) * 2 + 'Local' + chr(92) * 2 + 'Programs"',
     False, '正确的路径'),
    ('"The functional backend targets Windows.' + chr(92) + 'n"',
     False, '散文注释里的换行，不该报'),
    ('L"' + chr(92) + 'n"', False, '单纯的换行'),
    ('L"' + chr(92) + 't列宽"', False, '制表符'),
    ('"SOFTWARE' + chr(92) + 'Microsoft' + chr(92) + 'Windows' + chr(92) + 'CurrentVersion' + chr(92) + 'Run"',
     True, 'Run 键路径，单反斜杠'),
]

fails = 0
for lit, should_warn, note in cases:
    hits = B.scan_literal(lit)
    got = bool(hits)
    ok = (got == should_warn)
    if not ok:
        fails += 1
    shown = lit if len(lit) <= 56 else lit[:54] + '..'
    print('%s  %-58s 期望=%-5s 实际=%-5s  %s'
          % ('OK ' if ok else '失败', shown, should_warn, got, note))
    if hits:
        for esc, why in hits:
            print('        -> %s : %s' % (esc, why))

print('')
print('自检 %d/%d 通过' % (len(cases) - fails, len(cases)))
sys.exit(1 if fails else 0)
