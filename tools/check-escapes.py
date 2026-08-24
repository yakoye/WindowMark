r"""检查 C++ 字符串字面量里被吃掉的反斜杠。

背景：用 heredoc / sed 生成含反斜杠的源码时，shell 会把两个反斜杠折成一个，
于是 L"Software\Microsoft\Windows\DWM" 这种写法编译后变成
"SoftwareMicrosoftWindowsDWM"，注册表路径静默失效，功能一直走兜底分支。

/WX 能拦住非法转义（C4129），但拦不住碰巧合法的那种——
比如 "C:\new\file" 里的 \n 会变成真正的换行符，编译器一声不吭。
这个脚本专门补那个洞。

用法：python tools/check-escapes.py      有问题返回非零
"""
import io
import os
import re
import sys

# 允许单独出现的转义：控制字符、引号、数值转义。路径分隔符不在其列。
LEGAL_AFTER = set("abfnrtv0xuU'\"?\\")
# 这些即使合法也可疑：出现在一看就是路径的字面量里，多半是被吃掉的分隔符
SUSPICIOUS_LEGAL = set('nrtvabf')

LITERAL = re.compile(r'(?:L|u8|u|U)?"(?:[^"\\]|\\.)*"')
LINE_COMMENT = re.compile(r'//.*$')

PATH_PROBES = ('SOFTWARE', 'Software', 'HKEY', 'Windows', 'Program',
               'AppData', 'Local', 'Roaming', 'System32', 'CurrentVersion',
               'Microsoft', ':\\')


def looks_like_path(lit):
    """粗判是不是路径。

    要求同时满足两条，否则散文注释里出现 "Windows" 一词就会误报：
      1. 命中路径特征词
      2. 整个字面量不含空格 —— 真实的注册表键 / 目录路径不会有空格，
         而散文一定有
    """
    body = lit[lit.index('"') + 1:-1]
    if ' ' in body:
        return False
    return any(p in lit for p in PATH_PROBES)


def scan_literal(lit):
    """返回该字面量里可疑的转义 [(转义, 原因), ...]。"""
    out = []
    is_path = looks_like_path(lit)
    i = 0
    while i < len(lit) - 1:
        if lit[i] != '\\':
            i += 1
            continue
        nxt = lit[i + 1]
        if nxt == '\\':      # 正确写法，跳过整对
            i += 2
            continue
        if nxt not in LEGAL_AFTER:
            out.append(('\\' + nxt, '非法转义，编译时反斜杠会被丢弃'))
        elif nxt in SUSPICIOUS_LEGAL and is_path:
            out.append(('\\' + nxt, '路径里出现控制字符转义，像是被吃掉的分隔符'))
        i += 2
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    bad = []
    for dirpath, dirnames, filenames in os.walk('.'):
        dirnames[:] = [d for d in dirnames
                       if d not in ('build', '.git', '.vs', 'out')]
        for name in filenames:
            if not name.endswith(('.cpp', '.h', '.hpp', '.cc')):
                continue
            path = os.path.join(dirpath, name)
            with io.open(path, encoding='utf-8', errors='replace') as fh:
                for lineno, line in enumerate(fh, 1):
                    code = LINE_COMMENT.sub('', line)
                    for m in LITERAL.finditer(code):
                        for esc, why in scan_literal(m.group(0)):
                            bad.append((path.replace('\\', '/'), lineno,
                                        m.group(0).strip(), esc, why))

    if not bad:
        print('转义检查通过')
        return 0

    print('发现被吃掉的反斜杠：')
    for path, lineno, lit, esc, why in bad:
        shown = lit if len(lit) <= 70 else lit[:68] + '..'
        print('  %s:%d' % (path, lineno))
        print('      %s' % shown)
        print('      "%s" —— %s' % (esc, why))
    print('')
    print('改成双反斜杠，或改用原始字符串 R"(...)"。')
    return 1


if __name__ == '__main__':
    sys.exit(main())
