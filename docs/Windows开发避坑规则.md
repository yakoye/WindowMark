# Windows 开发避坑规则

这份文档只记**已经踩过、并且验证过结论**的坑。动手前先查这里，不要每次重新试一遍。

新增条目的要求：写清楚「现象 → 原因 → 该怎么做」，并注明是怎么验证的。没验证过的猜测不要写进来。

---

## 一、写文件用什么工具

### 铁律：内容里有反斜杠，就不要经过 shell

| 情况 | 用什么 | 不要用 |
|---|---|---|
| 内容含反斜杠 / 路径 / 正则 | `Write` 工具直接写整个文件 | heredoc、`sed`、`awk` |
| 要对已有文件做替换，且涉及反斜杠 | 用 `Write` 写一个 python 脚本再执行 | `sed -i`、`awk` |
| 文件比较长（几十行以上） | `Write` 工具 | heredoc（会被截断） |
| 纯 ASCII 的小改动、无反斜杠 | `sed` 可以 | — |

**为什么**：这个环境的 shell 会把 heredoc 里的两个反斜杠折成一个，哪怕定界符加了引号。

实际后果：`L"Software\\Microsoft\\Windows\\DWM"` 被折成单反斜杠，MSVC 按非法转义（C4129）把反斜杠丢掉，字符串变成 `SoftwareMicrosoftWindowsDWM`，注册表永远读不到，「强调色跟随系统主题」一直走兜底的蓝色，谁也没发现。

**写 python 脚本时自己也别踩**：脚本里凡是要表示反斜杠的地方，用 `chr(92)` 拼，或者整个用原始字符串 `r'...'`。正则的字符类 `[^"\]` 里那个单独的反斜杠会把 `]` 转义掉，必须写成 `[^"\\]`。

### 其他已知会坏事的写法

- **单引号数字分隔符**：C++ 的 `0x57'4D'50'4E` 会破坏 heredoc 的引号配对。写成 `0x574D504E`。
- **`awk`**：吃反斜杠，还吞过闭合的大括号。
- **`sed` 配合 `grep -n`**：把行号前缀 `123:` 一起写进了源文件。
- **`.bat` 文件内容必须全 ASCII**：`cmd` 按 GBK 解码，中文会让它崩掉。给用户的命令包成 `.bat` 时注意。

---

## 二、构建

- **MSVC 一律开 `/W4 /WX`**，GCC/Clang 开 `-Werror`。上面那个反斜杠 bug 编译器本来就报了 C4129，只是没人把警告当回事。
- **不要相信构建脚本自称「零警告」**。`reinstall.ps1` 原来只按 `$LASTEXITCODE` 判定，明明打印了 3 条 C4129，下一行照样写「编译通过（零错误零警告）」。改脚本时确认它真的把警告当失败。
- **改了公共头文件（如 `AppIdentity.h`）会触发全量重编**，这时候才会浮出之前被增量编译藏住的警告。别把「这次多了警告」当成新引入的。
- `tools/check-escapes.py` 补 `/WX` 拦不住的洞：碰巧合法的转义，比如 `"C:\new"` 里的 `\n` 会变成真的换行符，编译器一声不吭。`reinstall.ps1` 会在编译前跑它。

---

## 三、PowerShell

- `$pid` 是只读自动变量，不能赋值。换个名字。
- 给 `FindWindowW` 传 `$null`，P/Invoke 会变成 `""`，匹配不到窗口。传 `[NullString]::Value`，或者改用 `EnumWindows`。
- 本机是 PowerShell 7+，`&&` 和 `||` 可用。
- here-string 用 `@'...'@`（单引号、不插值），**闭合的 `'@` 必须顶格在第 0 列**，缩进就是语法错误。
- 非交互环境：不要用 `Read-Host`、`Get-Credential`、`Out-GridView`、`pause`。
- `-ErrorAction SilentlyContinue` 只压制错误输出，工具仍然报 exit 1。要真正忽略得 `try { ... -ErrorAction Stop } catch {}`。

---

## 四、Win32 行为（本项目实测）

### 窗口与层级

- **跨进程 owned window 是性能陷阱**。`CreateWindowExW` 传了别的进程的 `hWndParent`，宿主每移动一次都要同步往宿主线程跑一趟。Excel 拖动因此延迟 175ms、跟随比 0.024。改成 unowned + `WS_EX_TOPMOST` 之后：32ms、跟随比 0.971。
- **前台锁**：不拥有前台窗口的进程**无法抬升**窗口。`SetWindowPos(HWND_TOP)` 会返回 `TRUE` 然后什么都不做——日志里 34 次「成功」，窗口纹丝不动。
  - 创建时就带 `WS_EX_TOPMOST` 不算抬升，允许。
  - **降低**层级任何时候都不受限。
- `SetCapture` 要求调用方拥有前台窗口，给自己进程发个命令是伪造不出来的。
- DPI 感知与否，决定 `GetWindowRect` 返回逻辑像素还是物理像素。测量脚本一定要先 `SetProcessDpiAwarenessContext(-4)`。
- **`SetForegroundWindow` 在这台机器上不可靠**，即使配合 `AttachThreadInput`。它失败过四次，害我得出四个错误结论。别拿它当测量前提；改用「当前真实前台窗口」，或者反向验证（先找到结果，再倒推是哪个窗口）。

### WinEvent 钩子

- `WINEVENT_OUTOFCONTEXT` 的事件是**异步投递**的。等你收到 `MENUSTART`，目标程序早就把菜单画出来了 —— 这就是「第一次右击看不到菜单项」的根因。要提前装，不能等菜单打开才装。
- **`EVENT_OBJECT_INVOKED` = `0x8013`**。`0x8012` 是 `EVENT_OBJECT_ACCELERATORCHANGE`，我写错过一次。用宏，别抄数字。
- `EVENT_SYSTEM_MENUSTART` = `0x0004`，命名的是「菜单被打开的那个窗口」；`EVENT_SYSTEM_MENUPOPUPSTART` = `0x0006`，命名的是 popup 窗口本身（`OBJID_CLIENT`）。
- **`idObject` 不可靠**：真的 Alt+Space 给的是 `OBJID_WINDOW`，而用 `PostMessage(SC_KEYMENU)` 合成出来的给的是 `OBJID_SYSMENU`。不要按 `idObject` 过滤，改成校验窗口本身。

### 系统菜单

- `GetSystemMenu` + `InsertMenuItemW(menu, SC_CLOSE, FALSE, &info)` **跨进程可用，不需要注入**。本机 13 个窗口里 9 个成功。
- **UWP / WinUI 进不去**，这是平台硬限制，PowerToys 也一样：
  - `Windows.UI.Core.CoreWindow`：`GetSystemMenu` 直接返回 0。
  - `ApplicationFrameWindow`（ApplicationFrameHost 托管）：返回**非空**句柄，但 `GetMenuItemCount` 是 **-1**、`InsertMenuItemW` 报 **1401 ERROR_INVALID_MENU_HANDLE**。所以「句柄非空」不等于「能写」，要额外判 `GetMenuItemCount(menu) < 0`。
- 插进去的菜单项**活在对方窗口里，不在自己进程里**。自己退出时必须主动移除，否则会留下一个看着能点、点了没反应的条目。
- **`PostMessage(SC_KEYMENU)` 不会真的把菜单打开**，走的是另一条码路。拿它当探针会得出完全错误的结论——我因此连续两次修错了地方。要验证就真的发鼠标右键（`SendInput`）。
- 关掉 `TrackPopupMenu` 打开的菜单：给 **owner** 发 `WM_CANCELMODE`，不是给 popup 发 Esc。

### 全局快捷键

- `RegisterHotKey` 是**先到先得**，而且**输的一方不会收到任何通知**——注册失败只有返回值会说，
  用户看到的是「按了没反应」。所以默认不要占用任何组合，注册失败必须明确告诉用户。
- 想不碰键盘就验证「到底注册上没有」：另开一个进程去抢同一个组合，拿到
  **1409 `ERROR_HOTKEY_ALREADY_REGISTERED`** 就说明被占住了。启动前先抢一次确认组合本来是空闲的。
- 一定要带 `MOD_NOREPEAT`（0x4000），否则按住不放会反复触发。
- 快捷键**不夺取前台**，所以 `WM_HOTKEY` 到达时 `GetForegroundWindow()` 还是用户那个窗口。
  托盘菜单做不到这一点：菜单一打开，前台就是自己了。这是「快捷键能置顶当前窗口、菜单项不能」
  的全部原因。
- 录快捷键用子类化的 `EDIT`，不要用系统的 hotkey 控件：后者有自己一套「哪些组合合法」的看法，
  清不空，修饰键的显示顺序也不由你定。子类化时记得 `WM_GETDLGCODE` 返回 `DLGC_WANTALLKEYS`，
  否则 Tab / Enter / Esc / 方向键会被对话框管理器先吃掉——而这些恰恰都是可以绑定的键。

### 空的 std::function 是静默的

- `std::function` 没赋值时调用会抛 `bad_function_call`，所以代码里普遍写成
  `if (fn) fn();`——**于是「忘了接线」就变成了完全没有症状**：菜单项在、点击派发到了、
  什么都没发生、也没有报错。「全部取消置顶」就这样从上线起一直是死的。
- 对策两条：派发处 `else` 分支打一条日志；**启动时逐个检查所有回调**，缺一个就记一条，
  不要等到有人去点它。

### 手势要从自己的可见窗口开始

- 后台进程对**隐藏窗口**调 `SetForegroundWindow` + `SetCapture`，两步都可能被系统拒绝，
  而且失败是静默的。想开始一个鼠标手势，先放一个**真实可见的小窗口**让用户按下去：
  按在自己窗口上，这个进程就是前台、就持有鼠标，`SetCapture` 不可能失败。
- 从 `TrackPopupMenu` 的 `WM_COMMAND` 里直接 `SetCapture` 也不行。菜单模式收尾时会释放它自己
  的捕获，随之而来的 `WM_CAPTURECHANGED` 会把刚建立的 grab 掐掉。
- 拖动过程中，自己那个窗口必须对 `WM_NCHITTEST` 返回 `HTTRANSPARENT`，否则
  `WindowFromPoint` 永远只看得见它自己。捕获不受影响——被捕获的鼠标消息不走命中测试。
- `WS_EX_TOPMOST` 只是进了 topmost 组，**不等于在组的最前面**。弹出后补一次
  `SetWindowPos(hwnd, HWND_TOPMOST, ...)`，否则别的置顶窗口盖住它时用户那一下会按空。
- **不要用 `WM_KILLFOCUS` 当「用户改主意了」**。真被别的窗口盖住时，那一下正好落在它上面、
  它拿走焦点、你的窗口自杀——手势失败而屏幕上没有任何线索。Esc、右键、超时才是决定。

### 「这是不是一个真窗口」怎么判

- **类名不是判据**。Chrome、Claude、ChatGPT 和它们的悬停浮窗全都是 `Chrome_WidgetWin_1`。
  按类名排除会连正常窗口一起干掉。
- **尺寸不是判据**。最小化窗口的 `DWMWA_EXTENDED_FRAME_BOUNDS` 量出来是 183x34 这种小矩形，
  和一个 61x32 的浮窗没法用阈值分开。
- **样式位才是**。实测 13 个正常窗口全带 `WS_CAPTION|WS_THICKFRAME`，
  `WS_EX_NOACTIVATE` 和 `WS_EX_TRANSPARENT` 一个都没有；而浮窗两个都有。
  - `WS_EX_NOACTIVATE`：拒绝被激活。「用户会在这些窗口之间切换」是这类功能的前提，
    连激活都不接受的东西不可能是切换目标。
  - `WS_EX_TRANSPARENT`：命中测试透明，鼠标穿过去。用户点都点不到的窗口不是他要的窗口。

### 排查界面行为，不要在外面轮询

- 外部轮询工具**天生会漏**。30 秒采样 175 次 ≈ 170ms 一次，一个浮窗可能整个落在两次采样
  之间，于是「没抓到」和「没发生」长得一模一样。
- 事后再去查窗口的进程只会得到 `Idle`（PID 0）——窗口已经没了，
  `GetWindowThreadProcessId` 返回 0。这不是「有个叫 Idle 的进程」，是查晚了。
- 正确做法：**在程序内部、在那个行为发生的那一行**写日志。比如「新建轮廓」这个动作，
  每个被描边的窗口都必经此处，写在这里的记录按构造就是完整的。
- 给用户的排查工具要能**先打开、后复现**（实时跟随），而不是「打开即倒完历史然后退出」。

### 客户端自绘装饰（CSD）的窗口边界拿不到

- GTK 这类应用把投影画在**自己的窗口矩形内部**。实测 Czkawka（`gdkSurfaceToplevel`）：
  `GetWindowRect`、`DWMWA_EXTENDED_FRAME_BOUNDS`、`GetClientRect` **三者返回同一个矩形**，
  `GetWindowRgn` 也没设区域。对照普通窗口，DWM 边界会比 `GetWindowRect` 每边内缩 8px。
- 阴影区**不是** `HTTRANSPARENT`：从四边往里探，第 0 像素就命中窗口本身。
- `PrintWindow(hwnd, dc, 0)`（注意不是 `PW_RENDERFULLCONTENT`）能拿到真 alpha，但只对部分
  应用有效：Claude / ChatGPT 返回全透明位图，资源管理器和 PowerToys 上边是垃圾，
  而且耗时 8–59ms —— **它在逼对方重绘**。要用只能放进用户主动运行的工具里。
- 结论：**没有通用的自动办法**，只能按类名配置内缩量。最大化时要跳过（那时没有阴影）。
- GTK 窗口的 `GetSystemMenu` 返回 **0**（style 无 `WS_SYSMENU`）。标题栏右键那个菜单是
  GTK 自绘的仿制品，插不进去。

### 树控件的复选框

- **`TVS_CHECKBOXES` 不能写在 `CreateWindowEx` 的样式里**，必须创建之后、插入条目之前用
  `SetWindowLong` 加上。文档原话是「取决于时序，复选框可能显示为未选中」。
- 后果不是难看是**丢数据**：所有条目读回来都是未勾选，用户在面板里什么都不改按一下确定，
  就把整张表清空了。
- 教训：凡是「读出来—改—写回去」的界面，都要做**往返测试**——打开、什么都不改、按确定、
  比对文件。这一步抓到的，是肉眼看截图判断不出来的。
- 注意别把它误判成抓图问题。`PrintWindow` 确实不合成某些控件的状态图像，两个面板都拍成空的
  时候很容易得出「抓图假象」的结论——我就得出过，是往返测试推翻的。

### 说明文字的高度不要写死

- 固定 `noteH = 42` 只够两行，文案改长到三行就盖掉了下面的按钮。
- 用 `DrawText(DT_CALCRECT | DT_WORDBREAK)` 按实际文字量出来，布局跟着文案走，
  而不是文案迁就布局。

### 自家窗口的层级

- 只要这个程序会把别人的窗口放进 topmost 层，**自家所有对话框都必须带 `WS_EX_TOPMOST`**。
  普通窗口永远盖不过 topmost 窗口，否则设置窗口会开在被置顶的窗口下面，用户的反馈是
  「点了设置没反应 / 找不着」。
- `TaskDialog` 没有对应的标志，只能在 `TDN_CREATED` 回调里 `SetWindowPos(hwnd, HWND_TOPMOST, ...)`
  ——那是唯一能拿到它 HWND 的地方。

### 往别人窗口里加的东西，要考虑被强杀

- 插进别人系统菜单的条目**活在对方进程里**。自己被 `Stop-Process -Force` 杀掉时，
  清理代码根本没机会跑，条目就留在那儿了。
- 所以下次启动看到「已经有我们的条目」时，必须**把它认领进清理列表**，否则这个孤儿永远
  清不掉——每次都是「已存在，跳过」，每次优雅退出都漏掉它。

### 其他

- 系统强调色在 `HKCU\Software\Microsoft\Windows\DWM\AccentColor`，存的是 **ABGR**，红蓝字节和名字的顺序是反的。
- `DWMWA_CAPTION_BUTTON_BOUNDS` 对自绘标题栏不可靠：Electron / WinUI 会返回 0 宽度或者全 0。别用它猜别人的按钮位置。
- 开机自启动：`Run` 键之外还有 `StartupApproved\Run`，它的否决字节会**覆盖** `Run` 键。
  状态显示必须同时读两边；启用时先写 Run 命令，再把批准状态写成 `02`，任一步失败都要返回。
  只看/只写 `Run` 会出现「菜单已勾选，但登录时不启动」。真正的启动命令带 `--autostart`，
  由程序在 `%LOCALAPPDATA%\WindowMark\startup.log` 留下 `attempt → running`，不要再靠猜。

---

## 五、做测量时的纪律

- **先确认现象存在，再去解释它**。这个项目里「钩子开销」「z-order」「SetWindowPos 成本」「命中测试」「窗口数量」全都被我当过根因，全都被测量推翻。
- **探针必须走真实路径**。用合成消息代替真实输入，得到的是另一个系统的行为。
- **别拿会失败的前提做测量**（见上面的 `SetForegroundWindow`）。做不到就换验证方向。
- 抢鼠标/键盘的测试要**先跟用户打招呼**，每种情况跑 3 次就够。

### 采样界面像素时的两个坑（都踩过）

- **不要按自己算的坐标去采样**。控件尺寸经过 DPI 缩放，`Scale(18)=23`、`Scale(7)=9`，
  实际间距 32px，而我按比例反推出来是 30px——到第 6 格就偏了 12px，读出一串看不懂的颜色，
  差点当成绘制 bug。**扫一整行像素、按色块归并**，画成什么样就读成什么样。
- **背景色不要从左上角取**。选中环画在色块外面，(0,2) 正好落在环上是黑的，于是
  「非背景」判定把整条都算进去了，7 个格子合并成 6 段乱码。从**右上角**取背景。
- 想看整体效果，`PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT=2)` 直接抓成 PNG，
  不抢焦点也不碰鼠标，比逐点采样可靠得多。
