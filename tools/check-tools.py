# 把 tools 目录下每个脚本里「用了但没定义」的名字找出来。
#
# 存在的理由很具体：Python 的 NameError 只在执行到那一行才报，而这些工具里最要紧
# 的代码（事件回调、后台线程）恰恰只在特定时机才跑。bench-focus-latency.py 就这样
# 带着一个未定义的 GW_HWNDNEXT 交出去过，观察满 30 秒才发现每个线程都在抛异常。
#
#   python tools\check-tools.py
import ast
import builtins
import io
import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))


class Scope(ast.NodeVisitor):
    """收集模块里所有被绑定的名字，以及所有被读取的名字。

    不做作用域分析——这些脚本都是平铺的，模块级名字加函数内的局部名字合在一起
    比对已经够用，宁可漏报也不误报。
    """

    def __init__(self):
        self.bound = set(dir(builtins))
        # 模块级的内置变量，dir(builtins) 里没有
        self.bound.update({'__file__', '__name__', '__doc__', '__package__',
                           '__spec__', '__loader__', '__builtins__'})
        self.used = []          # (name, lineno)

    def _bind_args(self, args):
        for a in args.args + args.kwonlyargs + getattr(args, 'posonlyargs', []):
            self.bound.add(a.arg)
        if args.vararg:
            self.bound.add(args.vararg.arg)
        if args.kwarg:
            self.bound.add(args.kwarg.arg)

    def visit_FunctionDef(self, node):
        self.bound.add(node.name)
        self._bind_args(node.args)
        self.generic_visit(node)

    visit_AsyncFunctionDef = visit_FunctionDef

    def visit_Lambda(self, node):
        # lambda 的参数照样是绑定，漏掉它会把 sorted(..., key=lambda kv: -kv[1])
        # 里的 kv 报成未定义。
        self._bind_args(node.args)
        self.generic_visit(node)

    def visit_ClassDef(self, node):
        self.bound.add(node.name)
        self.generic_visit(node)

    def visit_Import(self, node):
        for a in node.names:
            self.bound.add((a.asname or a.name).split('.')[0])

    def visit_ImportFrom(self, node):
        for a in node.names:
            self.bound.add(a.asname or a.name)

    def visit_ExceptHandler(self, node):
        if node.name:
            self.bound.add(node.name)
        self.generic_visit(node)

    def visit_Global(self, node):
        self.bound.update(node.names)

    def visit_Name(self, node):
        if isinstance(node.ctx, (ast.Store, ast.Del)):
            self.bound.add(node.id)
        else:
            self.used.append((node.id, node.lineno))


bad = 0
for name in sorted(os.listdir(TOOLS)):
    if not name.endswith('.py') or name == os.path.basename(__file__):
        continue
    path = os.path.join(TOOLS, name)
    with io.open(path, encoding='utf-8') as fh:
        src = fh.read()
    try:
        tree = ast.parse(src, filename=name)
    except SyntaxError as exc:
        print('%-28s 语法错误 第 %s 行: %s' % (name, exc.lineno, exc.msg))
        bad += 1
        continue

    scope = Scope()
    scope.visit(tree)
    missing = [(n, ln) for n, ln in scope.used if n not in scope.bound]
    # 同一个名字只报第一处，免得一个笔误刷屏
    seen = set()
    unique = []
    for n, ln in missing:
        if n not in seen:
            seen.add(n)
            unique.append((n, ln))
    if unique:
        bad += 1
        print('%-28s 用了没定义的名字：' % name)
        for n, ln in unique:
            print('    第 %-4d 行  %s' % (ln, n))
    else:
        print('%-28s OK' % name)

print()
if bad:
    print('%d 个文件有问题。' % bad)
    sys.exit(1)
print('全部通过。')
