# Changelog

## v0.4.3

### 修复：勾选“开机启动”后，重启仍不自动运行

原来的菜单只检查 `HKCU\...\Run` 里有没有 `WindowMark`，没有检查 Windows 另外维护的
`Explorer\StartupApproved\Run`。任务管理器或「设置 → 应用 → 启动」禁用程序时，Windows
会保留 Run 命令，只把批准字节改成禁用；因此菜单仍显示勾选，实际登录时却不会运行。

现在状态判断同时验证三件事：Run 值存在、类型和命令有效、批准状态是 `02`（或尚无批准记录）。
任何已记录但不是 `02` 的状态都按禁用处理，避免再显示一个虚假的勾选。

启用流程也收紧了：

- 先写完整的 `"...\\WindowMark.exe" --autostart` 命令，再写批准状态，顺序不再颠倒；
- 批准键不存在时会创建，而不是静默放弃；
- 安装器和托盘菜单共用一个返回成功/失败的实现；
- 托盘写入失败会弹出明确错误，不再无声失败。

### 新增：登录启动诊断

开机命令带 `--autostart`。只有从该命令进入时，程序才会向
`%LOCALAPPDATA%\WindowMark\startup.log` 追加 `attempt`、`running` 或明确的失败阶段。
这样重启后能直接区分「Windows 根本没有执行启动项」与「执行了，但程序初始化失败」。

### 测试

新增 `windowmark_autostart_tests`，在独立的临时 HKCU 测试键下验证正常启用、Windows 否决、
损坏数据、重新启用、重复禁用和命令行生成，不会修改真实的 WindowMark 启动项。

## v0.4.2

### 新增：边框排除应用

托盘 →「窗口边框」→「排除应用...」，打开一个勾选面板，**勾上的应用不画边框**。
放在菜单里而不是设置窗口里，和书签的「选择应用/窗口...」对称：这两个回答的都是
「这个功能作用在哪些窗口上」，和设置页里那些数值是不同性质的问题。

应用层的勾选写进配置长期有效，单个窗口的勾选只在本次运行内有效（窗口句柄跨次重启不稳定，
存下来只会误伤继承了同一个数字的窗口）。

**键用的是可执行文件路径，不是类名，也不是标题。** 这一点是这个功能能用的全部原因：本机实测
有 **6 个不同的 exe 共用 `Chrome_WidgetWin_1`** 这一个类名——

```
chrome.exe / Typora.exe / Claude.exe / ChatGPT.exe / Feishu.exe / 墨鱼阅读.exe
```

按类名排会把六个一起干掉；按 exe 路径排，勾掉墨鱼阅读，chrome 毫发无损。

标题更不能用：它随用户切换页面而变，而且实测那条飞书云文档的标题在第一个可见字符之前
带了**恰好 50 个不可见字符**（零宽连接符、双向控制符之类），和 `Types.h:66` 早年记下的
测量结果一字不差。

面板复用书签那套（`WinSelectionDialog`），但有两处必须不同：

- **去掉「窗口数少于 2 就不列」的过滤**。同应用书签对单窗口没有意义，边框有：墨鱼阅读
  就只有一个窗口，而它正是这个列表要处理的对象。
- **勾选的含义反过来**。入口叫「排除应用」，那么勾上就必须是「排除」，否则用户面对的是
  一个双重否定。只有显示层反转，`AppSelectionModel::enabled` 在任何地方都只有一个含义，
  Coordinator 不需要知道面板是哪一个。

**选中一行时对应窗口会在屏幕上高亮**，所以不需要像 `WindowMarkInspect` 那样给每个窗口贴编号。

被排除的应用**一旦被置顶仍然会画边框**——那是置顶生效的唯一提示，和 `border.enabled` 关掉时
的处理一致。

配置项 `border.excluded_apps`，与书签的 `selection.disabled_apps` 分开：不想要边框和不想要书签
是两回事。

### 修复：勾选面板的复选框一直是坏的（书签面板同样中招）

`TVS_CHECKBOXES` 被写在 `CreateWindowExW` 的样式里。这是有明确文档的坑：这个样式必须在
**创建之后、插入任何条目之前**用 `SetWindowLong` 设，否则「取决于时序，复选框可能显示为
未选中」。

后果不是难看，是**丢数据**：所有条目读回来都是未勾选，于是在书签面板里原样按一次「应用」，
就会把所有应用的书签全部关掉。

是往返测试抓到的——打开面板、什么都不改、按「应用」、比对配置文件。这个测法值得对每个
「读出来—改—写回去」的界面都做一遍。

### 修复：说明文字换到第三行会顶掉「取消」按钮

说明区的高度原来写死 42px，只够两行。改成用 `DT_CALCRECT` 按实际文字量出来，
以后改文案不会再撞。

## v0.4.1

### 新功能：窗口置顶

第三个功能，和书签、边框一样是独立的一块，互不牵连。三种触发方式：

- **标题栏右击 →「❏置于顶层」**。`GetSystemMenu` + `InsertMenuItemW` 往对方系统菜单里插
  一项，点击通过 `EVENT_OBJECT_INVOKED` 收到。全程公开 API，**不注入 DLL、不改对方的窗口
  过程、不猜对方的按钮位置**——这条是设计前提，不是实现细节。
- **托盘 →「抓取窗口置顶...」**。DeskPins 那样的十字准星。
- **全局快捷键**，默认不设。

置顶只在本次会话内有效，退出时逐个恢复原状。本来就置顶的窗口不会被误取消——注册表里
存的是「置顶之前它是什么样」，重复置顶不会覆盖这个记录。

置顶窗口一律画高亮边框，**不管 `border.enabled` 是不是关着**。边框就是「钉住了」的唯一
反馈，把它挂在一个默认关闭的功能上，等于按了没反应。

#### 快捷键

- 默认**不占用**任何组合键。`RegisterHotKey` 先到先得且输的一方不会收到通知，所以不主动抢。
- 在「置顶设置」里直接按下组合键录入，退格清除；必须带修饰键。
- 被占用时**明确弹框**说清楚，不留一个按了没反应的快捷键。
- 作用于当前前台窗口。托盘菜单做不到——菜单一开前台就是 WindowMark 自己了。

#### 已知限制

UWP / WinUI 窗口加不上菜单项，平台限制：`CoreWindow` 的 `GetSystemMenu` 返回 0；
`ApplicationFrameWindow` 返回非空句柄但 `GetMenuItemCount` 是 -1、插入报
`1401 ERROR_INVALID_MENU_HANDLE`。PowerToys 同样进不去。用准星或快捷键。

### 修复

- **强调色跟随系统一直没生效**。`L"Software\Microsoft\Windows\DWM"` 写成了单反斜杠，
  `\M` `\W` `\D` 都不是合法转义，MSVC 直接丢掉反斜杠，注册表永远读不到，
  一直在用兜底的 `#0078D4`。编译器本来报了 C4129，但 `reinstall.ps1` 只按 exit code 判定，
  照样打印「零错误零警告」，就这么混过去了。
  - 现在 MSVC 开 `/W4 /WX`，警告即错误
  - `reinstall.ps1` 有警告就不安装
  - 新增 `tools/check-escapes.py`，补 `/WX` 拦不住的那种（`"C:\new"` 里碰巧合法的 `\n`），
    编译前自动跑；`tools/check-escapes-selftest.py` 是它自己的自检
  - 详见 `docs/Windows开发避坑规则.md`
- **第一次右击标题栏看不到菜单项，要右击第二次才有**。`WINEVENT_OUTOFCONTEXT` 的事件是
  异步投递的，等收到 `MENUSTART` 时对方早把菜单画出来了。改成两处提前装：启动时枚举一遍
  现有窗口，之后靠 `EVENT_SYSTEM_FOREGROUND` 接住新出现的。
- **菜单项在程序退出后残留**。条目活在对方窗口里而不是自己进程里，不主动移除就会留下一个
  看着能点、点了没反应的条目。退出时逐个收回；上一次是被强杀留下的孤儿也会被认领后清掉。
- **设置窗口被置顶窗口压在下面，找不着**。普通窗口永远盖不过 topmost 窗口，所以自家的
  设置 / 选择 / 关于三个窗口都加了 `WS_EX_TOPMOST`。

### 新增：自绘阴影内缩（GTK 等 CSD 应用）

GTK 应用（例如 Czkawka）把投影画在**自己的窗口矩形内部**，那圈边距完全透明，于是边框看起来
离窗口很远。新增设置：

```ini
tracking.shadow_insets=gdkSurfaceToplevel:22,12,22,38
```

格式 `类名:左,上,右,下`，多个应用用 `|` 分隔。**窗口最大化时自动忽略**——那时没有阴影，
而缓存本来就按尺寸变化重算，所以最大化和还原都会自动取到正确的值。

配套 `measure_shadow_inset.bat`：双击后把鼠标停在目标窗口上，它打印出可以直接粘贴的那一行。

**为什么只能手工配、不能自动测**。三条路都实测走不通：

| 手段 | 实测结果 |
|---|---|
| 几何接口 | `GetWindowRect`、`DWMWA_EXTENDED_FRAME_BOUNDS`、`GetClientRect` 对 Czkawka **返回同一个矩形**；`GetWindowRgn` 没有设置区域 |
| 命中测试 | 从四边往里探，**第 0 像素就命中**窗口本身，阴影区不是 `HTTRANSPARENT` |
| `PrintWindow` + alpha | 对 Czkawka 可用（22,12,22,38，103/138 条扫描线零离群），但对 Claude / ChatGPT 返回全透明位图、对资源管理器和 PowerToys 上边是垃圾数据，且耗时 8–59ms——它在**逼对方重绘** |

最后一条虽然对 GTK 有效，但它会影响别的应用，与「应用尽量做的相对独立」冲突，所以只放进
手动运行的测量工具，常驻程序不做。

顺带修掉一个不一致：`BuildWindowInfo` 用的是 `ExtendedFrame`，而 `QueryFrame` 用的是
`FrameFor`——同一个数字有两个来源，导致修正只在拖动途中生效、窗口初始几何却没有。现已统一。

### 已知限制：GTK 窗口没有系统菜单项

Czkawka 这类 GTK 窗口的 `GetSystemMenu` 返回 **0**（style 里没有 `WS_SYSMENU`）。
标题栏右键弹出的那个「还原/移动/大小/最小化/最大化/关闭」是 **GTK 自己画的仿制菜单**，
不是 Win32 系统菜单。要往里加项就必须动 GTK 内部，与独立性原则冲突。
这类窗口用**准星抓取**或**全局快捷键**置顶，两者均已实测可用。

### 修复：悬停浮窗会被当成真窗口

ChatGPT 里悬停消息弹出的时间戳小浮窗（61x32），**既被画了边框，也进了书签列表**——
书签按可执行文件分组，于是它作为第二个窗口挂在了 ChatGPT 名下。

判据加在 `IsEligibleTopLevelWindow`，那是书签、边框、置顶共用的唯一入口，改一处三处都好：

```
WS_EX_NOACTIVATE   窗口拒绝被激活。点书签就是去激活一个窗口，
                   连激活都不接受的东西，本质上不可能是切换目标。
WS_EX_TRANSPARENT  命中测试透明，鼠标直接穿过去。用户根本点不到的窗口不是他要的窗口。
```

这两个位是**抓到真凶之后**定的，不是猜的。实测那个浮窗：
`style=0x96000000`（只有 `WS_POPUP`，无 caption、无边框）、
`ex=0x08200028`（`NOACTIVATE | NOREDIRECTIONBITMAP | TRANSPARENT | TOPMOST`）、标题为空。
同时量了 13 个正常窗口：**全部带 `CAPTION|THICKFRAME`，`NOACTIVATE` 和 `TRANSPARENT` 一个都没有。**

同样重要的是**没有**采用的两种做法：

- **按类名排除**会把 Chrome、Claude、ChatGPT 一起干掉——三个正常窗口和那个浮窗都是
  `Chrome_WidgetWin_1`。
- **按尺寸排除**会把所有最小化窗口干掉——最小化时 DWM 扩展边界量出来是 183x34。

两者都不是「这是不是一个真窗口」的属性，这两个样式位才是。

### 新增：无损的描边记录

`WinBorderBackend::Apply` 新建轮廓时写一行日志（受 `diag.on` 开关控制），内容是创建当时的
进程名、类名、尺寸位置、完整 style / exStyle、是否最小化、标题；撤销轮廓时也写一行，
于是能看出存活时长。

**每个被描边的窗口必经这里，所以这份记录是完整的。** 之前用外部轮询工具排查这个问题失败过：
30 秒采样 175 次约 170ms 一次，一个浮窗可能整个落在两次采样之间，「没抓到」看起来和
「没画过边框」一模一样，而且事后再去查进程只会得到 `Idle`（PID 0，因为窗口已经没了）。

配套 `show_border_log.bat`：双击打开后**实时跟随**，去把浮窗弄出来，它被描边的那一刻就会打印。

### 抓取置顶改成拖动准星

原来是两种入口：拖托盘图标，或点菜单「抓取窗口置顶...」后再单独点一下目标窗口。两种都不好用，
现在合并成一种：菜单里点「⊕抓取窗口置顶...」，屏幕上出现一个准星手柄，**按住它拖到目标窗口上
松开**，那个窗口就置顶。Spy++ 的找窗口工具是同一个手势。

**托盘图标拖拽已移除。** 图标经常被折进溢出面板里，而且占用左键就得把「普通点一下图标」和
「开始拖拽」区分开，代价大于收益。

手柄不只是换了个交互，它修的是根因：原来两条路径都要先对一个**隐藏的 0×0 窗口**调
`SetForegroundWindow` 再 `SetCapture`，而后台进程这两步都可能被系统拒绝；菜单那条还要跟
`TrackPopupMenu` 收尾时释放捕获抢时间（一旦收到 `WM_CAPTURECHANGED` 就把 grab 掐掉了）。
现在是用户在**我们自己的可见窗口**上按下鼠标才开始，`SetCapture` 不可能失败。

其他细节：

- 拖动中手柄对 `WM_NCHITTEST` 返回 `HTTRANSPARENT`，否则 `WindowFromPoint` 永远只看得见它自己。
  捕获不受影响——被捕获的鼠标消息本来就不走命中测试。
- 弹出后补一次 `SetWindowPos(HWND_TOPMOST)`。`WS_EX_TOPMOST` 只是进了 topmost 组，不等于在组的
  最前面，别的置顶窗口盖住准星时那一下会按空。
- Esc、在手柄上右键、15 秒无操作都可取消。

### 修复

- **「全部取消置顶」从来没生效过**。`handlers.onUnpinAll` 在 `WinMain` 里压根没赋值，而派发处
  有 `if (*handler)` 兜底，于是点了完全没反应、也没有任何报错。现在接上了，并且：
  - 处理器为空时会打日志，而不是静默返回
  - 启动时逐个检查所有处理器，缺一个就记一条，不用等到有人去点
- **抓取手柄会在按下之前自己消失**。它原来在 `WM_KILLFOCUS` 时销毁自己，可一旦有别的置顶窗口
  盖住准星，那一下就落到那个窗口上、它拿走焦点、手柄随即自杀——手势失败且屏幕上没有任何线索。
  丢焦点不是「用户改主意了」，Esc、右键、超时才是。

### 颜色改成色卡选择

三处颜色设置（边框活动/非活动、置顶高亮）不再让人手填 `#RRGGBB`，改成一排色卡：
**6 个预设 + 1 个自定义**。

- 第一格是各自原来的默认值（`#6274E7` / `#7080AA` / 跟随系统强调色），升级上来的配置
  不会突然在色卡里找不到自己。
- 其余五格是红、橙、黄、绿、紫。**白色不在其中**——白边框在浅色桌面上等于没画。
- 最后一格「⋯」打开系统取色器，任何颜色都能选，16 个自定义槽位跨次保留。
- 键盘可用：Tab 进焦点，左右方向键切换，空格/回车确认，Home/End 跳到两端。

**顺带修掉一个显示错误**：置顶的「高亮颜色」原来显示 `#00000000`。因为「跟随系统强调色」
内部存的是 `0`，格式化出来就是黑色——用户看到的和实际生效的完全不是一回事。现在那一格
直接画出当前真正在用的颜色，旁边写「跟随系统 #0078D4」，值是每次读注册表实时取的。

`SystemAccentColor()` 也从 `WinBorderBackend.cpp` 挪到了 `WinUtil`。那条注册表路径是个满是
反斜杠的宽字符串，让它存在第二份，就是上面那个 bug 卷土重来的方式。

### 调整

- 置顶高亮线宽默认 **15 → 10**。

## v0.3.7

### 托盘菜单重排

```
WindowMark          ← 灰色标题，程序名放这里
书签              >
窗口边框           >
──────────
暂停所有 / 启用所有   ← 主开关，标签说的是「点了会怎样」
开机启动
关于
──────────
退出
```

- **主开关**：一次关掉/打开两个功能，不用分别进两个子菜单。标签跟着状态换（运行中显示
  「暂停所有」），所以不需要勾选标记去解读。
- **开机启动**：放顶层而不是塞进某个子菜单——它切换的是整个程序。**全新安装后默认不勾选**，
  安装器读的就是当前注册表状态，干净的机器上读到的是「没有」。
- **弹出位置**改为右边缘对齐光标（`TPM_RIGHTALIGN`）。托盘在右下角，原来左对齐等于往屏幕
  边缘挤，每次都被系统的边界钳制推回来。

**宽度**：Win32 原生菜单的宽度**就是最长那一项的文字宽度**，不改成自绘就没有别的办法，
而自绘会丢掉 Win11 的圆角和亚克力。所以是靠缩短标签做的：程序名移到标题行后，
「关于 WindowMark...」→「关于」、「退出 WindowMark」→「退出」；
「开机自启动」→「开机启动」；子菜单里「选择参与的应用/窗口...」→「选择应用/窗口...」——
最后那一项原本是整个菜单系统里最宽的，而子菜单是靠父菜单左边缘右对齐的，所以只有它决定
「书签」面板往左伸多远，明显比旁边的「窗口边框」面板远。

（开机启动先做进了两个设置对话框，位置不对，已改到托盘菜单。设置对话框那条「不经过
Settings、直接读写外部状态」的字段通道留着了，理由没变——见下。）

### 关于框加了作者与邮箱

作者 yekoye，邮箱 yuxiang_163com@163.com。

### 开机启动为什么不写配置文件

**不写进 `settings.conf`。** 真相只有一份，在 `HKCU\...\Run` 下。Windows 允许用户从任务
管理器和「设置 - 应用 - 启动」里关掉启动项，如果我们自己的配置文件里再存一份，那份会永远
坚称相反的事。菜单每次弹出都现读注册表，代价是一次开键。

顺带处理了一个隐藏坑：Windows 给每个启动项另存了一个**批准字节**（`StartupApproved\Run`）。
用户一旦从任务管理器禁用过，这个字节会压过 Run 项——条目还在，我们读起来是「已启用」，
但开机就是不跑。所以从程序里勾选时会一并清掉这个否决位，否则勾了等于没勾。

### 设置对话框高度改为算出来的

两个页面的高度原本是手写常量（404 / 516），每加一个字段都得肉眼重新量一遍——而这正是会被
跳过的一步，失败的样子是控件画到「确定」按钮上面。现在按字段表走一遍和布局代码相同的
分组/行距算术得出。

### UI 改动改成自动核对

以前 UI 只能靠人眼看。这一版加了两个脚本化的检查，都不碰鼠标：

- **设置对话框**：用 `WM_COMMAND` 直接调出来，枚举子控件，核对「最低的输入控件」与「按钮
  行顶端」的间距。两个页面各有 47px 余量。
- **托盘菜单**：给托盘窗口发消息把菜单弹出来，用 `MN_GETHMENU` 拿到 `HMENU`，逐项读出文字、
  命令 id 和勾选状态；再验证点击 → 注册表写入 → 菜单勾选跟着变 → 再点 → 删除。

## v0.3.6

### 拖动卡顿的真正原因：跨进程 owner

书签条创建时把宿主窗口当成了 owner（`CreateWindowExW` 的 `hWndParent` 传的是另一个进程
的 HWND）。宿主每移动一次，Windows 都要维护这个「被拥有窗口」的层级关系，而 owner 在另
一个进程，这需要一次到 WindowMark 线程的**同步往返**。Excel 的拖动循环就卡在这上面。

量化（8 字形注入拖动，x 半幅 780px、y 半幅 400px，每秒 2 个来回，5 轮交错取中位数）：

| 条件 | 光标→窗口延迟 | 窗口跟随率 |
|---|---|---|
| 有 owner（修复前） | **174.7ms** | **0.024** |
| 无 owner | 17.3ms | 0.976 |
| 书签窗口完全不存在 | 14.0ms | 0.966 |

跟随率 = 窗口走过的路程 ÷ 光标走过的路程。0.024 意味着窗口几乎没跟上，整段整段地漏掉往返。

**修复后的总体效果**（书签和边框都开）：

| 条件 | 延迟中位 | 跟随率中位 |
|---|---|---|
| 书签+边框 | **32ms** | **0.971** |
| 只开书签 | 24ms | 0.969 |
| 只开边框 | 30ms | 0.958 |
| WindowMark 未运行 | 10ms | 0.983 |

从 +165ms 降到 +22ms，跟随率与基线已无法区分（0.971 对 0.983）。

### 去掉 owner 之后如何维持层级

owner 机制原本免费换来一件事：**允许把窗口放到前台窗口之上**。去掉之后必须自己解决，
过程中踩了两个坑，都是「返回成功但什么都没做」：

1. **前台锁**。非前台进程发起的「抬升」类 z 序调用会被静默忽略——`SetWindowPos` 返回
   TRUE、不设标志、不移动窗口。日志里 34 次「置顶成功」而窗口纹丝不动。
   解法：**创建时就带 `WS_EX_TOPMOST`**。创建不算抬升，是允许的。而书签条只在宿主是前
   台窗口时才存在，置顶正好是它该在的位置。
2. **遍历预算被隐藏窗口吃光**。向上找锚点窗口的步数上限写死 64，而实测宿主上方压着
   **144 个隐藏窗口**，永远走不到可见窗口就耗尽预算。解法：步数与「实际插入尝试次数」
   分成两个预算，跳过隐藏窗口几乎免费，不该占用配额。边框的同一处也一起修了。

### 只给可见的书签建窗口

`drawer.active_window_only` 打开时每组只有一个书签条可见，但过去给组里**每个**成员都建了
窗口——常见桌面上 12 个，其中 11 个永远不显示。现在不可见就不建。

副作用是切换同组窗口时会销毁旧的、新建新的，而这恰好让「创建时置顶」在每次切换后都自动
生效。

### 边框层级：修好了三处，退回了一处

**遍历预算**同书签一样拆成步数与插入尝试两个配额，否则永远走不到可见窗口。

**不再锚定到置顶窗口**。`SetWindowPos` 会把插到置顶窗口下面的窗口一并提升，一次这样的
锚定就让一个普通窗口的边框变成了「总在最前」——实测到一个边框跑到 z=14 且置顶位为真，
浮在所有无关窗口之上。书签条现在是置顶的，恰好最容易被第一个撞上。

**前台窗口的边框改为紧贴宿主下方**。前台窗口之上放不了东西（`HWND_TOP` 会返回成功但什么
都不做），过去这里什么都不做，结果边框留在原地——实测到一个 Excel 的边框卡在**另一个
Excel 窗口下面**，那正是「边框不完整」的样子。改成 `SetWindowPos(边框, 宿主)` 插到宿主
正下方：这是降低操作，不受限制；代价只有压在窗框上的那 1px，其余 3px 和「盖过其它所有
窗口」都保住了。

**退回的那一处**：曾把 `SyncZOrder()` 改成只在激活或可见性变化时才跑（窗口移动不可能改变
层级）。道理成立但结果是错的——层级也会因为我们收不到的事件而漂移，一旦漂移就再没有东西
把它拉回来。收益是 34ms → 30ms，在噪声里；代价是边框会时不时缺一块。不划算，退回。

### 排查过程中被证伪的假设

外部测量能定位到「书签有问题」，但看不进去，几个从外面推出来的结论都被实测推翻了：

- 全局 WinEvent 钩子的存在本身 —— 空转钩子进程与基线完全一致
- Z 序操作、`SetWindowPos` 的次数和耗时 —— 18 秒只调用 69 次，撑不起 69ms
- 命中测试（`WS_EX_TRANSPARENT`）—— 715px vs 696px，无差别
- 重绘与文字重排 —— 拖动全程 **0 次**
- CPU 与锁争用 —— WindowMark 12 秒只烧 0.25 秒，Excel 用量与基线相同
- 窗口数量 —— 12 个减到 1 个，依然 748px

真正定位靠的是把链路拆成「注入坐标→系统光标→窗口移动」两段分别测量：A 段恒为 0ms，
说明系统输入管线正常，问题全在 B 段，也就是 Excel 自己被卡住。

### 版本与构建时间戳

`kProductVersion` 升到 0.3.6。另外每次构建自动写入 `BuildStamp.h`，关于框和
`reinstall.ps1` 都会显示构建时间并校验安装的二进制与刚构建的哈希是否一致——
「我装的到底是不是最新的」不再靠记忆回答。

### 诊断计数器

`WinOverlayBackend` 里加了一组分阶段计数器，默认关闭，设 `WINDOWMARK_DIAG=1` 时才写
`%LOCALAPPDATA%\WindowMark\diag.log`。记录 Apply 次数与耗时、SetWindowPos 的耗时分布、
重绘与重排次数、以及 `SyncZOrder` 每次的决策和结果。层级这条路径从进程外完全看不见，
这次两个「返回成功但没生效」的坑都是靠它挖出来的。

## v0.3.5

### 移动只更新位置，不再整个重查

A location event means one thing: the window moved. It was being handled by re-running the
full `QueryWindow` — class name, process path, title, work area, DWM frame — about **0.7ms
of cross-process calls**, measured. One 80-step Excel drag delivers **274** of them, and
274 x 1.2ms lands almost exactly on the 328ms of CPU that was being burned.

`IWindowBackend::QueryFrame` returns just the frame, reusing the DWM inset the backend
already caches. A resize still takes the full path, because that one can cross a monitor
edge or flip the maximized state.

Measured with a realistic drag — figure-of-eight, 1200px across and 400px down, two round
trips a second for three seconds, seven runs per configuration:

| 配置 | 卡住中位 | 散布 | 平均滞后 | CPU |
|---|---|---|---|---|
| WindowMark 未运行 | 2.5% | 0.7~4.8% | 3.6px | — |
| **只书签** | **3.1%** | 0.8~4.7% | **4.1px** | 484ms |
| 只边框 | 2.8% | 0.7~**75.8%** | 76.0px | 484ms |
| 书签+边框 | 6.8% | 0.8~7.8% | 42.4px | 688ms |

**Bookmarks are now level with not running WindowMark at all.**

### 仍未解决：边框在快速拖动下偶发卡死

Borders still take the unthrottled path — a `SetWindowPos` on a layered window per location
event, which is how their tracking was matched against tacky-borders. The median is fine
(2.8% against a 2.5% baseline) but **one run in seven stalled 75.8% of samples with a mean
lag of 76px and a peak of 1216px**, i.e. the outline dragged the window to a near halt for
a stretch. Both configurations with borders on show the same 1216-1220px peak. This is the
remaining cause of what an Excel drag feels like, and it needs its own investigation.

### 试过又撤掉的两件事

- **Bookmarks on the border's unthrottled move-only path.** Added a `SetWindowPos` per
  location event without removing any throttled work; CPU rose, smoothness did not change.
- **Hiding both decorations during a drag** (`EVENT_SYSTEM_MOVESIZESTART`/`MOVESIZEEND`).
  Hiding worked — verified the strip really disappeared — but the stutter did not move,
  because hiding only removed the *drawing*; the per-event query behind it was still being
  paid. Fixing the query is what actually worked, so the hiding came out again.

  It did surface a real bug on the way through: `EmitPending` had a hardcoded list of event
  kinds, so the two new ones had their bits set and were **never emitted** — the feature
  silently did nothing, and those map entries never went away. The list is now exhaustive
  with a `static_assert` that turns the next omission into a compile error.

### 拖动时隐藏书签（已撤销，保留记录）

`performance.hide_bookmarks_while_dragging`, on by default, in 书签设置 → 性能 → 拖动时隐藏.
Hooks `EVENT_SYSTEM_MOVESIZESTART` / `MOVESIZEEND`: the strip for the window being dragged
is hidden and left alone until the drag ends, then rebuilt at the new position. Borders are
deliberately not covered — they hug the window edge where hiding would be obvious, and
their drag tracking was matched against tacky-borders on purpose.

**What the measurements actually support, and what they do not.** Seven runs of an
80-step Excel drag per configuration, counting steps where the window did not move at all:

| 配置 | 卡住中位 | 均值 | 散布 | CPU |
|---|---|---|---|---|
| WindowMark 未运行 | 1/80 | 0.4 | 0~1 | — |
| 只边框 | 1/80 | 0.9 | 0~3 | 391ms |
| 只书签 | 1/80 | 0.9 | 0~2 | 359ms |
| 书签+边框 | 3/80 | 2.7 | 1~5 | 516ms |

- Each feature on its own is at or barely above the baseline. An earlier three-run pass
  said bookmarks were four times worse than borders; seven runs made that difference
  vanish. It was noise, and three runs was not enough to see it.
- **Both together is where it becomes visible** — median 3 against 1, reproduced in every
  run.
- The hide-while-dragging change is in and does what it says (the events were confirmed to
  fire), but **this benchmark cannot show that it helps**: its noise floor is the same size
  as the effect, and the medians did not move across four passes.

### 不做的事，以及为什么

Bookmarks were also put on the border's unthrottled move-only path, then taken back off.
It added a `SetWindowPos` per location event — 274 of them in one 80-step drag — without
removing any of the throttled work, so CPU rose and smoothness did not change. The strip
sits outside the window, where 30fps is fine.

### Defaults

- Version 0.3.2 -> 0.3.5.

### 关于对话框

- Uses the app's own icon instead of the stock blue "i", via TaskDialog — MessageBox only
  takes the system icons.
- **Only one at a time.** Every dialog here is modal to the *hidden* tray window, and
  disabling that window blocks input to it but not the tray icon's callback message, so the
  menu stayed live and could stack a second copy of any dialog. A shared guard now covers
  the settings, selection and rename dialogs too; the about box additionally raises the
  existing one rather than silently ignoring the click.

## v0.3.2

### 排除窗口自己就能配

- **`tracking.exclude_classes`** — extra window classes to ignore entirely, no bookmark and
  no border, on top of the built-in list. Edit it under 窗口边框 → 边框设置... → 排除窗口,
  comma separated, applied immediately.

  This is the difference between the exclusions being *my* problem and being solvable. The
  built-in list was measured on one Windows build with one set of IMEs and neither
  travels: another Windows version renames its shell classes, another IME brings its own
  candidate window. Without this, every such window meant a code change and a rebuild.

  Verified end to end: a test window that reliably gets an outline stops getting one the
  moment its class is added to the setting, and gets it back when removed.

### WindowMarkInspect.exe

The diagnostic is a real program now instead of a PowerShell script, built to
`build\Release\WindowMarkInspect.exe`. It watches for 20 seconds, then **numbers every
outline** — a yellow badge on each one still on screen, the same numbers in a table with
the class name beside them — and spells out where to paste the class name. Flyouts that
have already closed keep their number in the table without a badge, which is the only way
to catch an IME candidate list.

It shares nothing with the app but the window class name. An earlier script version
reimplemented the filtering rules, drifted from the code, and confidently told me to
exclude a class that was already excluded; this one only looks at the outlines that are
actually on screen.

### Icon

`res/wmiicon.ico` is now the tray icon and both settings windows' title-bar icon. Loaded
with `LoadImage` at the small-icon metric rather than `LoadIcon`, so Windows takes the
16px image out of the multi-size file instead of shrinking the 256px one. Replacing the
file is the whole procedure — the `.rc` is generated by CMake with the path substituted in,
so nothing needs an include path or an id change.

### Defaults

- `drawer.short_name_chars` 3 -> **4**.

## v0.3.0

Adds window borders, so one tray app now covers both features.

### Window borders

Inspired by [tacky-borders](https://github.com/lukeyou05/tacky-borders), reimplemented
natively rather than bundled. That project is Rust and WindowMark is C++, so shipping it
would have meant a second process with its own WinEvent hooks and its own tray icon —
twice the event handling for something the existing infrastructure already does. Window
tracking, layered-window rendering and DWM frame queries were all already here; a border
is just another overlay.

- Outlines every top-level window, including single-window apps that never get a bookmark
  strip. Separate colours and opacities for the active and inactive window.
- Corner radius follows the system by default, asking DWM per window
  (`DWMWA_WINDOW_CORNER_PREFERENCE`); Windows 10 has no rounding and gets square corners.
  Can be forced square or round.
- **Completely independent of bookmarks**: its own switch, its own settings window, its
  own tray submenu. The two share only window tracking.
- Off by default (`border.enabled`). Sizing and colours follow tacky-borders' conventions:
  `border.offset` is negative-inward, `border.corners` is `auto|square|round|round_small|
  custom`, and colours take `#RGB`, `#RGBA`, `#RRGGBB` or `#RRGGBBAA` with alpha carried
  in the colour rather than a separate opacity setting.
- Not carried over: gradients, animations and effects. See ROADMAP.md for what else from
  tacky-borders is still outstanding — `window_rules`, `initialize_delay`,
  `follow_native_border`.

Getting this to match tacky-borders took three rounds of measurement; the findings are
worth recording because each one contradicted the obvious guess:

- **Z-order, twice.** Outlines were first being buried under the very windows they
  outlined, leaving only the pixels of overhang visible — which looked like "some edges
  are missing". `SetWindowPos(A, B)` places A *after* B in z-order, i.e. below it, so
  passing the target was exactly backwards. Fixed by inserting below whatever is directly
  above the target. Owner windows were not an option, for the same cross-process reason
  the bookmark overlays ran into.

  That fix then failed on **Task Manager specifically**, permanently: its outline sat 14
  slots too low and no amount of moving, resizing or refocusing brought it back. The
  window directly above Task Manager is its own hidden `Default IME` window — every thread
  gets one, and it is *owned* by the window it serves. Windows will not slot anything
  between an owner and a window it owns, so that `SetWindowPos` returned
  `ERROR_ACCESS_DENIED` and did nothing, silently, forever. The walk now skips invisible
  siblings and keeps going if a call is refused. Stepping over hidden windows is free:
  they paint no pixels, so being above them looks the same as being below them. Audited
  across every window on the desktop, before and after deliberately scrambling the
  z-order: 0 outlines misplaced, and 0 cases of a *visible* window ending up sandwiched
  between an outline and its target.

  Always-on-top windows turned out to work for free — Windows promotes a window to
  topmost when it is inserted below a topmost one — but the reverse does not happen. When
  an app turns always-on-top *off*, the outline keeps the flag and stays stranded in the
  topmost band, floating over 20-30 unrelated windows and never coming down. Only
  `HWND_NOTOPMOST` clears it, so the target's topmost state is now matched explicitly on
  every sync. Measured across the on -> off -> move -> on cycle: the outline stays exactly
  one slot above its target throughout, and its topmost flag tracks the target's.
- **Lag.** Geometry events are throttled to 33 ms for the bookmarks' sake, which is
  invisible for a strip outside the window but very visible on an outline hugging its
  edge, so `IWindowBackend` gained an unthrottled geometry sink. That was necessary but
  not sufficient. Measured head-to-head against tacky-borders on the same window and the
  same drag, this build tracked 50/80 frames perfectly against its 80/80.
  The cause was **an outline being four strip windows**: every location event had to
  reposition four windows instead of one. Caching the cross-process style and DWM frame
  queries changed nothing (62% -> 60%); disabling bookmarks entirely changed nothing
  (46/80 either way), ruling out UI-thread contention. Going back to one window per
  outline took it to **80/80 with 0px mean error — level with tacky-borders**.
- **Memory.** One window-sized layered bitmap per outline is ~9 MB for a maximized
  window, and a full desktop reached **~240 MB**. The fix is not to shrink the windows but
  to stop each one owning a bitmap: `UpdateLayeredWindow` copies what it is handed, so a
  single process-wide scratch bitmap serves every outline in turn. Private memory is now
  **~55 MB** for 12 outlines, below tacky-borders' 73 MB, on 4 threads against its 32.
  Working set is higher (122 MB vs 67 MB) because `UpdateLayeredWindow` keeps a
  system-side copy per layered window that `SetLayeredWindowAttributes` does not — see
  ROADMAP.md.
- **The seam.** `border.width` defaults to 4 (was 3), keeping `border.offset` at
  tacky-borders' -1, so the outline reaches 3px past the window and covers its last pixel.
  Windows draws a 1px frame of its own around each window; if the outline stops one pixel
  short of it — which is what `offset: 0` does — that frame shows through as a grey seam
  between the outline and the window: measured `#646765` on Explorer, `#4F5255` on Chrome,
  and clearly visible at 8x. Overlapping by one pixel hides it. Measured across
  `w3/off0`, `w3/off-1`, `w4/off-1` and `w5/off-1` on both a Chrome-style and a standard
  Win11 frame: every configuration with `offset: -1` gives an outline that is the
  configured colour end to end, with no seam.
- `SetWindowPos` now passes `SWP_NOSENDCHANGING | SWP_NOREDRAW | SWP_NOCOPYBITS`, and
  border windows are `WS_DISABLED` on top of `WS_EX_TRANSPARENT`, both following
  tacky-borders.

### Bookmarks

- `drawer.bottom_expanded_extent` 150 -> **120**: how wide a bottom tab grows on hover.
- **The active row tab has its own height** (`drawer.bottom_active_thickness`, default
  23px). It was hard-wired to `drawer.thickness`, so the only way to change it was to
  change `thickness` — which also shrank the resting tabs and the strip, and meant the
  value silently went back to 34 whenever `thickness` was touched. A hovered tab now grows
  to the active height rather than past it, and the strip is sized to the active tab
  instead of to `thickness`.
- **Titles are stripped of characters that draw nothing** before they become labels. A
  collapsed tab shows the first few *characters* of a title, and a Chrome page was
  measured carrying fifty zero-width code points (ZWNJ, BOM, the invisible maths
  operators) before its first real glyph — so the tab rendered correctly and completely
  blank. `SanitizeTitle` drops the invisibles and any leading whitespace; every visible
  character is untouched, including CJK and 4-byte emoji, and malformed UTF-8 passes
  through rather than being swallowed.

### Settings

- Bookmark and border settings are now **two separate windows**, reachable from two tray
  submenus (书签 / 窗口边框). They share the dialog implementation but not their field
  lists, so either feature stays liftable on its own.
- New field 底部横排 → 激活高度 for the active tab's height.
- **Bookmarks have an on/off switch of their own** (`drawer.enabled`, 书签 → 启用书签),
  the same shape as `border.enabled`. It replaces a runtime-only flag that the tray menu
  toggled, so the choice now survives a restart and the tray item and the checkbox cannot
  disagree — both read and write the same setting.
- **Both dialogs are sized to their content.** Every width is derived from the widest
  thing it has to hold rather than set to a round number: the label column fits
  「仅在当前窗口显示」, the number fields fit four digits, the drop-downs fit 「跟随系统」.
  The border page also drops to a single column — seven fields in two columns left half
  the window empty. Bookmarks went 700x500 -> **506x516**, borders 700x250 -> **274x340**.
  The footer moved onto its own line above the buttons, which is what lets the border page
  be narrow enough for just the three buttons.
- 「仅在当前窗口显示」 is now 「仅当前窗口」. At eight glyphs it was the single label
  holding the whole bookmark page's label column 26px wider than anything else needed.
- Two constraints the layout now enforces rather than hopes for: the page can never be
  narrower than its own button row (three buttons drew on top of each other at 260px), and
  the label and hint columns are measured **per page** — sharing them made each page pay
  for the other page's longest string.

### Excluded windows

- **The IME no longer gets an outline, and no longer flickers.** 「Windows 输入体验」 is a
  full-screen `Windows.UI.Core.CoreWindow` belonging to `TextInputHost`, which DWM keeps
  *cloaked* while idle — so it passed the cloaked check and looked like an ordinary window.
  Typing uncloaks it, and so does the keyboard-layout flyout, which put a screen-sized
  outline up for as long as the candidate list was open. Cloak, uncloak, cloak: that is
  what the flicker was.
- **The IME candidate bar and the layout switcher too.** These turned out to be two more
  windows, found by watching which windows actually received an outline over 30 seconds
  rather than by guessing: `Shell_InputSwitchTopLevelWindow` (「Input Flyout」, 480x410) is
  the switcher, and the candidate bar is an **`ApplicationFrameWindow`** — the same class
  that hosts every UWP app.
- **`ApplicationFrameWindow` is judged by owner, not by class.** Excluding the class
  outright would strip the outline from 设置, 计算器 and every other UWP window. The shell
  reuses the class for its own chrome, and what separates the two is which process owns
  it: an app's frame belongs to `ApplicationFrameHost.exe`, the shell's to `explorer.exe`.
  Verified against live windows: 计算器 and 设置 (`ApplicationFrameHost`, pid 22828) keep
  their outlines; the two untitled `explorer` (pid 8344) frames are excluded.
- The class exclusion list grew from four entries to ten. `Windows.UI.Core.CoreWindow` and
  `XamlExplorerHostIslandWindow` are also what tacky-borders ships as defaults. Making the
  list configurable is ROADMAP item 1.

### Tooling

- `rebuild_and_install.bat` — double-click to build, test, uninstall and reinstall in one
  go. `-Fresh` also deletes `settings.conf`, which is the only way a changed *default*
  becomes visible: an existing file overrides it.
- `list_tracked.bat` / `list-tracked.ps1` — double-click and it watches for 30 seconds,
  then lists every window that **actually received an outline**, transient flyouts first,
  with class names ready to paste back. It does not replicate the C++ filter — it finds
  the real `WindowMark.WindowBorder` windows on screen and works out what each one is
  around, so it cannot drift out of step with the code. Two things it took to get right:
  matching by rect size rather than by centre (a 2560x1400 maximized window and a
  2560x1440 IME host have centres 20px apart, and the tool confidently named a class that
  was *already excluded*), and falling back to `WindowFromPoint` when the sizes disagree,
  because the candidate bar resizes on every keystroke and the outline lags it by tens of
  pixels.
- `check_border.bat` / `check-border.ps1` — border diagnostics. Reports where the outline
  window actually is, whether it is above its target, the colour of every sampled pixel on
  all four edges, and for the wrong ones, which window is covering them. It exists because
  "this edge is missing" has two completely different causes and guessing between them
  wasted time twice.

### Removed

- **The `Ctrl+Alt+W` global hotkey.** `RegisterHotKey` claims a combination process-wide
  for the whole session: whichever app asks first wins and every other one silently loses
  it. Not a trade worth making for a switch that is two clicks away in the tray menu,
  which now shows 启用书签 with a check mark instead of 显示/隐藏书签.
- Colours are entered as `#RRGGBB`; an unparseable value is reported rather than silently
  becoming black.
- The tray menu is grouped rather than flat, and the border switch shows a check mark.

## v0.2.0

### Bookmarks always show text

`drawer.short_name_chars` was treated as a literal glyph count and the font size was
fixed at 13px, but a tab is only as tall as the drawer settings make it. Measured with
DirectWrite, 13px needs **17.3px of line height** — more than the 16px a default collapsed
row tab provides. Text that overflows its layout rect is not trimmed by
`D2D1_DRAW_TEXT_OPTIONS_CLIP`, it disappears completely, so a bookmark would render as a
blank coloured block while its neighbours read fine.

- The font size is now derived from each tab's own height, and tabs differ (the active
  one is at full thickness, the rest rest at part of it), so this is decided per tab.
- Labels are still trimmed to the measured width, with `short_name_chars` as an upper
  bound rather than a target.
- Verified exhaustively against real DirectWrite metrics: **550 combinations** of 11 tab
  heights (10–80px, covering the smallest a `thickness` of 20 can produce), 5 collapsed
  widths (24–100px) and 10 labels spanning Latin, CJK, Japanese, Cyrillic, emoji and
  single characters — 0 cases where text would be empty, too wide or too tall.

### Defaults and settings UI

- **`placement` now defaults to `bottom`.** `auto` chooses a side from whichever has more
  free space, so the strip jumps between left and right as a window is dragged and you
  have to hunt for it. A fixed edge stays put.
- **Fixed the settings window opening off-screen.** It was centred on `owner`, which is
  the hidden tray control window — a 0x0 window parked at (0,0) — so the maths produced
  negative coordinates. It now centres on the work area of the monitor under the pointer,
  clamped so the title bar always stays reachable, and sizes itself with that monitor's
  DPI rather than the tray window's.
- **Fixed controls overlapping the buttons.** The right column ran to ~464px while the
  button row sat at 428px. Groups are rebalanced across the columns and the window is
  taller; a guard now reports the overflow instead of silently drawing on top.
- The settings window is brought to the foreground on open — its owner is hidden, so
  nothing did that for it and the dialog could appear behind another window.

### About

- Tray menu gains **关于 WindowMark...**, showing version, executable and config paths,
  and what the app deliberately does not touch. The settings window footer shows the
  version too.

## v0.1.0-rc4

Adds the settings UI, bookmark renaming and configurable transparency, and fixes two
installer defects found while verifying them.

### Settings

- **Settings dialog**, reachable from the tray menu and from a bookmark's right-click menu. It exposes every configurable value — appearance, bottom row, hover preview, behaviour, performance — grouped in two columns, with a 恢复默认值 button. Out-of-range numbers are reported rather than silently clamped, so a typo cannot quietly become a different setting.
- Changes **apply immediately and are written to `settings.conf`** — no restart. `IOverlayBackend`/`IPreviewBackend` gained `UpdateSettings`, and `Coordinator::UpdateSettings` pushes the edit through and re-applies the models.
- New `drawer.transparency` (percent, 0 = fully opaque, the default). It applies to every tab equally; the active one is still told apart by geometry, not opacity.

### Bookmarks

- **Fixed collapsed tabs rendering blank for some labels.** `drawer.short_name_chars` was applied as a literal character count, but three CJK glyphs are roughly twice as wide as three Latin ones: at the default 44px bottom tab only ~35px is usable, and a three-CJK label measures 39px. Overflowing text combined with `D2D1_DRAW_TEXT_OPTIONS_CLIP` disappeared entirely rather than degrading, so one tab looked empty while its neighbours read fine. The label is now measured with DirectWrite and trimmed to what actually fits; `short_name_chars` is an upper bound rather than a target, and labels that already fit are untouched.
- **Right-click a bookmark** for 重命名 / 设置.
- **Renaming** overrides the window title on that bookmark. It is deliberately session-only, for the same reason per-window selection is: a generic OS window has no reliable cross-session identity, so a persisted name would eventually attach itself to the wrong window. Clearing the field restores the title.
- Bottom/top rows now anchor **inside** the host's own edge whether or not it is maximized. Previously a restored window's row hung below the window while a maximized one sat inside it, which flipped the root edge and drew those tabs upside down.

### Installer fixes

- **Silent install could hang forever.** `ShellExecuteExW` was called without `SEE_MASK_FLAG_NO_UI`, so when a launch failed it popped its own modal error box and waited for a click that a `/S` install will never get. All three call sites now suppress that UI and report failures through return values instead.
- **Install-over-running intermittently left nothing running.** Waiting on the old process handle was not enough — if `OpenProcess` failed the wait was skipped entirely, and a predecessor still mid-exit kept the single-instance mutex. The new instance then saw that mutex, concluded it was a second launch, and exited silently, which looked exactly like the installer not starting the app. The installer now waits for the mutex to actually be released, retries the launch, and verifies the process is still alive afterwards; `/S` returns a non-zero code if it is not.
- Fixed a dangling pointer in the uninstall dialog: `pszMainInstruction` pointed at a temporary `std::wstring`'s `c_str()`, so the title rendered as garbage.

## v0.1.0-rc3

First revision actually built and run on Windows. Fixes what that run exposed.

### Build

- Fixed MSVC `std::max` ambiguity in `WinSelectionDialog.cpp`: `RECT` members are `LONG`, so `std::max(0, ...)` could not deduce a single `_Ty`. Same class of bug rc2 fixed in `WinPreviewBackend.cpp`.

### Layering and legibility

- **Bookmarks now appear only on the foreground window** (`drawer.active_window_only`, on by default). Overlays are owned popups of windows belonging to *other* processes, and Windows does not keep cross-process owner/owned z-order in sync, so strips belonging to background windows could float above whatever was actually in front. Restricting to the active host makes the owner always the foreground window, which is the position the overlay should occupy anyway. Set the key to `false` for the old every-window behaviour.
- Every bookmark is squared off at its **root edge** — the edge it grows out of — so it meets the window seamlessly there, like a bookmark slipped between pages. Which edge that is depends on placement: side tabs reach outward from the window, so the root is the edge nearest it; row tabs grow inward from the window's own boundary, so the root is that outer edge. This holds for the active tab too: it cannot overhang its root, and rounding it there made it look like it was floating off the edge.
- The active bookmark is instead set apart by **geometry alone** — it is longer (`drawer.active_extra_extent`) and it sits over the window edge where the others stop at it, so it reads as lying *on top of* the window while the rest read as slotted in *behind* it. No outline, no shadow, no opacity difference: every tab is filled identically, which keeps the strip quiet and the draw path to a single rounded-rect fill per tab.
- Bottom/top rows size independently from the sides (`drawer.bottom_collapsed_extent`, `drawer.bottom_expanded_extent`): there a tab's extent is its width, so the side numbers never transferred. Row tabs also rest at half thickness against the window edge and grow toward the host on hover (`drawer.bottom_collapsed_thickness`, 0 = half of `drawer.thickness`).

### Rendering

- Fixed bookmark tabs rendering as black blocks offset from their own window. The Direct2D render target inherited the system DPI (120 on a 125% display) while every rectangle from `LayoutEngine` is in physical pixels, so all drawing was scaled 1.25x and pushed off the right edge; the exposed area showed the cleared background, which an `ALPHA_MODE_IGNORE` HWND render target renders as opaque black. The render target is now pinned to 96 DPI.
- Overlays are now layered windows presented with `UpdateLayeredWindow` from a premultiplied-alpha DIB. Tabs have real per-pixel transparency and antialiased corners; the `SetWindowRgn` approximation is gone.
- Compact tabs center their label and drop the ellipsis, which at 30px was consuming a glyph that the tab needed.
- The tray selection panel now gets Common Controls v6 visuals.

### Performance

Idle cost is 0.02% CPU. The work that mattered was window dragging, where a geometry
event fires roughly 30 times a second after throttling and each one previously rebuilt
everything. Measured on a 7-window group with synthetic drag plus cursor traffic, that
path went from ~20% of one core to ~15%.

- `WinEventProc` now rejects on `objectId`/`childId` before anything else. `EVENT_OBJECT_LOCATIONCHANGE` fires for every mouse move (`OBJID_CURSOR`) and every child object on the desktop, and each of those rejects was paying for a `GetWindowThreadProcessId` cross-process call first. The call was also redundant — the hooks use `WINEVENT_SKIPOWNPROCESS`.
- Executable paths are cached per `HWND` (invalidated when the pid behind it changes, pruned on full enumeration). Dragging no longer performs an `OpenProcess`/`QueryFullProcessImageName` round trip, a UTF-8 conversion and a lowercase pass ~30 times a second per window.
- `QueryProcessPath` uses an inline `MAX_PATH` buffer and only falls back to a heap buffer on `ERROR_INSUFFICIENT_BUFFER`, instead of allocating 64 KB per call.
- One shared `ID2D1DCRenderTarget` and text format for all overlays, replacing one HWND render target plus one text format per bookmarked window.
- Item rectangles are computed into a reused buffer, and labels are converted to UTF-16 once per model change instead of once per frame. Neither the animation tick nor mouse tracking allocates any more.
- A geometry event that only moves a host window skips re-rendering and re-uploading the layered bitmap, and overlays whose position and visibility are already correct skip `SetWindowPos`/`ShowWindow` entirely. Dragging one window no longer disturbs its siblings.

### Install / uninstall

- Replaced `scripts\install.ps1` and `scripts\uninstall.ps1` with `WindowMarkSetup.exe` and `WindowMarkUninstall.exe`, both native GUI executables that also accept `/S` for scripted use.
- **A running WindowMark is no longer an error anywhere.** Installer and uninstaller post a registered quit message straight to the tray control window, wait for the process to exit, and only force-terminate as a fallback. Installing over a running copy restarts it.
- Launching `WindowMark.exe` a second time no longer shows an error dialog; it notifies the live instance, which shows a tray balloon, and exits 0. `--purge` on a running instance now asks it to close instead of refusing.
- The installer registers a **设置 - 应用** entry, a Start menu shortcut, and an optional startup entry; the uninstaller removes all of them and can also purge user data. The uninstaller runs itself from `%TEMP%` so it can delete its own install directory, and cleans that copy up afterwards.

### Defaults

- `drawer.collapsed_extent` 52 -> 30, `drawer.short_name_chars` 4 -> 3.
- New keys: `drawer.active_window_only`, `drawer.active_extra_extent`, `drawer.bottom_collapsed_extent`, `drawer.bottom_expanded_extent`, `drawer.bottom_collapsed_thickness`. An existing `settings.conf` is never rewritten on upgrade, so add them by hand or delete the file to regenerate it.

### Known gaps, deferred to the next revision

- No settings UI yet: everything is edited in `settings.conf` and picked up on restart.
- No right-click menu on a bookmark.
- Tab opacity is fixed; the inactive hold-back is not configurable.
- Hover preview exists (`preview.enabled`) but has not been verified on Windows, and its
  window uses the same cross-process owner arrangement that the overlays just moved away
  from, so it may sit behind the host window.

### Tests

- `tests/core_tests.cpp` used `assert()` for calls with side effects. Under `NDEBUG` those calls vanished, so the Release test binary skipped `Coordinator::Start()`, `Settings::Save()` and `DrawerState::Tick()` entirely and then crashed on an empty `std::function`. Replaced with a `CHECK` macro that always evaluates and always verifies, in every configuration.

## v0.1.0-rc2

Bug-fix and selection-control revision based on the first Windows build feedback.

- Fixed MSVC `std::max` ambiguity in `WinPreviewBackend.cpp` caused by mixing Win32 `LONG` values with `int` literals.
- Added a native tray entry: **选择需要书签的应用/窗口...**.
- Added a platform-neutral selection model in Core; the Windows UI is only a frontend for that model, so macOS can provide its own UI later.
- Application-level enable/disable is persisted in `settings.conf` using the backend-provided application identity.
- Individual-window enable/disable is supported for the current process session; it is intentionally not persisted because generic OS windows do not have a reliable cross-session identity.
- Disabled windows are removed both as bookmark hosts and bookmark targets; a group must still have at least two enabled windows before any overlay is created.
- Added selection filtering and settings round-trip tests.
- Kept the same `v0.1.0` target version; this is `rc2`, not a feature-version bump.

## v0.1.0-rc1

First runnable architecture revision.

- Platform-neutral Core with `IWindowBackend`, `IOverlayBackend`, and `IPreviewBackend`.
- Windows event-driven window discovery/tracking with `SetWinEventHook`.
- Same-app grouping by executable path.
- One bookmark overlay per independent window; every overlay contains the same bookmark set for that app group.
- Rounded pastel-rainbow bookmark tabs.
- Platform-neutral DrawerState: compact by default, hovered item expands independently, click switches immediately.
- `Self` and `Active` visual states are separate.
- Auto layout: normal windows prefer left-side bookmarks; maximized windows use bottom bookmarks.
- Hover preview via DWM thumbnail, one preview at a time; width/height configurable.
- Geometry-event throttling/coalescing to reduce drag/resize overhead.
- Tray control and `Ctrl+Alt+W` quick hide/show.
- Portable-style install/uninstall scripts with optional full purge.
- macOS backend boundary/scaffold included from the first revision.
