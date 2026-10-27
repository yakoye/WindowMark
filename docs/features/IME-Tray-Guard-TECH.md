# 输入法占位保护：阶段一技术预研

日期：2026-10-24。结论等级：**只读诊断原型可运行；原生占位尚未证明可行；不进入正式功能集成。**
这不等于已经证明“不可能实现”。控制台显隐对照已经收到，隔离布局实验通过；
当前仍缺 Win10 实测、原生稳定宿主及其恢复验证。后续更新见文末。

## 1. 已经实测的事实

开发机系统构建 `26300.9711`，显示版本 `26H2`，当前是 RDP 会话 3，本地控制台会话 4。
注册表 ProductName 仍是 `Windows 10 Pro`；不能据此把这次采样算作 Windows 10 验证。
带兼容性 manifest 后 Explorer 固定文件版本读得 `10.0.26100.9711`。
最早未带 manifest 的原型曾读到 `6.2.26100.9711`，该字段不用于兼容性结论。

观察到的层次（UIA，省略非目标子树）：

```text
Shell_TrayWnd
  Windows.UI.Composition.DesktopWindowContentBridge
    Windows.UI.Input.InputSite.WindowClass
      Taskbar.TaskbarFrameAutomationPeer
      SystemTray.NormalButton / AccentButton / OmniButtonLeft / OmniButtonRight
        Image / TextBlock
  TrayNotifyWnd
  ReBarWindow32
```

当前可见系统托盘按钮大量共用 `AutomationId=SystemTrayIcon`，通知按钮共用 `NotifyItemIcon`。
因此 **不能按这个 ID 找到两个按钮就开始修改**，也不能按从左数第几个硬编码。
所观察到的这些按钮 `transform_available=0`；UIA 没有向本探针暴露可调整宽度的 Transform 模式。
启发式 IME 分类在本次样本为 0：仅代表未识别，不能证明系统不存在 IME。
本次前台不是已知显隐复现场景，不能用于“切换远程客户端后仍稳定”的验收。

额外只读检查：任务栏所属 Explorer PID 20748 已加载 `Taskbar.View.dll 2608.26001.200.0`、
`SystemTray.dll 2608.26001.100.0`、`Windows.UI.Xaml.dll 10.0.26100.8972` 以及两种
Microsoft.UI.Xaml 版本。仅知道操作系统 build 不足以选择内部结构适配器。

## 2. 路线判断与依据

| 路线 | 已知能力/限制 | 当前决定 |
| --- | --- | --- |
| Shell_NotifyIcon | 管理通知图标，没有提供固定系统原生 IME 区域宽度的参数 | 不能用新增透明图标替代 |
| 外部 UIA | 可观察边界与身份；调整大小取决于 provider 是否暴露对应模式 | 用于采证；本机未发现可写宽度模式 |
| 外部 HWND 调整 | 可看到 TrayNotifyWnd 等，但不等同于拥有 XAML 布局约束 | 不调用 SetWindowPos 强行维持宽度 |
| 原生 XAML 稳定宿主 | 理论上可研究保留空间，同时不改原生图标状态 | 首选研究路线，宿主/生命周期未确认 |
| XAML 诊断进程内组件 | 官方提供调试附加入口，但需要进程内 DLL | 尚未注入；必须先定义最小目标和可恢复边界 |
| 假图标/屏幕覆盖层/重启服务 | 与用户目标不符或违反安全要求 | 不开发、不当作退路交付 |

微软的 [Shell_NotifyIcon 文档](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw)
列出图标增删改等消息；它并不是系统输入法容器布局接口。
[UIA Transform 文档](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nn-uiautomationclient-iuiautomationtransformpattern)
说明其能力由对应模式提供。以上“本机不支持调整大小”是实测；不能推导为所有 Windows 永远不可修改。

XAML 的 [Collapsed 定义](https://learn.microsoft.com/en-us/uwp/api/windows.ui.xaml.visibility?view=winrt-26100)
明确不保留布局空间。因此只对一个即将 Collapsed 的节点写 Width/MinWidth，不能单独构成占位方案。
还要确认被折叠的是子节点、宿主还是集合项目，以及系统是否移除/重建整个项目。
UIA 看不到一个节点，不能区分这些情况。

[InitializeXamlDiagnosticsEx](https://learn.microsoft.com/en-us/windows/win32/api/xamlom/nf-xamlom-initializexamldiagnosticsex)
是 XAML 诊断入口，文档明确有进程内 DLL 参数；“有调试 API”不代表存在受支持的任务栏扩展协议。
若研究该路线，需要独立实验组件、明确附加/分离寿命、目标 UI 线程、失败处理和原值恢复，不能直接并入自启。

公开的 [Windhawk 系统托盘图标修改源码](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-tray-system-icon-tweaks.wh.cpp)
提供了研究线索：语言主图标与附加图标使用不同视觉内容类型；实现依赖进程内视觉树和私有符号，
新版本还存在 SystemTray.dll 与 Taskbar.View.dll 的宿主变动。
这些仅是候选线索，不是本机 IME 目标已确定的证据。
该源码标注 GPLv3；WindowMark 是 MIT，本轮没有复制其实现或引入其 Hook。

## 3. Windows 10 / 11 双轨

| 平台 | 本轮状态 | 下一个必要证据 |
| --- | --- | --- |
| Windows 10 22H2 x64（优先验证目标） | 无真机证据 | 控制台 HWND/UIA、两个图标显隐、布局宿主 |
| Windows 11 常用稳定构建 x64 | 无本地控制台证据 | 同上，另记录任务栏修改工具 |
| 当前 build 26300.9711 x64 RDP | 只读采集和启动器实测通过 | 不能代替稳定版/本地控制台/Win10 验收 |

只读 EXE 使用的 PMv2 DPI 接口要求较新的 Windows 10（1703+）；更早版本不在这份原型的承诺范围。
ARM64、32 位、旧版 Windows 10 与第三方任务栏未验证；探针能启动也不代表保护适配成功。
Win10 可能采用不同托盘控件结构；不得把 Win11 类名和 Hook 原样套过去。

## 4. 正式模块接入计划（尚未实现）

- 配置：在 `Settings.h/.cpp` 中新增独立设置组，默认关闭；保存不覆盖既有功能配置。
- 主面板：当前 `HomeFeature` 仅四个窗口功能。任务栏作为独立区域，不复用书签/边框开关。
- 命令：主面板与托盘共用启用/关闭/恢复命令；显示实际状态而不只显示 enabled 意图。
- 生命周期：`WinControlWindow` 已接收 TaskbarCreated，目前只恢复自己的托盘图标；未来通知独立管理器重探测。
- 会话：已有 `WinBorderBackend` WTS 会话监听，但不可把保护功能绑到边框开关。
- 看门狗：`Coordinator::WatchdogTick` 面向窗口跟踪；保护管理器独立健康状态，不重启 Explorer。
- 状态机：Disabled → Checking → Active；实例/DPI变化先失效旧句柄，Recovering 重新探测；失败进入 Unsupported/ErrorSafeDisabled。
- 异常退出：需要证明宿主仍存活时如何回滚，不得仅以析构函数在正常退出被调用作为崩溃恢复证据。

## 5. 进入布局原型前的判据

1. 在明确的系统与显示环境中确定两个原生项目，记录各自稳定身份、父容器和测量单位。
2. 得到“两个可见/一个消失/两个消失/恢复”前后对照，不能只靠客户端进程名推断。
3. 如 UIA 不足，才设计有边界的进程内观察；未知结构与不匹配版本不写入。
4. 对稳定宿主的最小布局修改保存 unset/local/binding 原始状态，并证明透明区域不命中鼠标。
5. 证明正常恢复与异常恢复，再提交占位可行性结果，随后正式集成。

目前缺口是第 1～4 项的真实证据，不是编译器或诊断包不可用。

## 6. 收到本地控制台证据后的更新

对端会话1、WTS协议0、1920×1080、DPI120；构建26300.9711。
60个有效样本中，mstsc前台18次均为10个可见托盘按钮；其余42次均为12个。
20次集合转换每次均有40px和55px的两个相邻按钮一起增减，左侧4个既有按钮相应移动95px，
右侧6个既有按钮的边界不变。该现象是实测，不再是假定。
用户截图3/4确认了本机最底部任务栏「中」「拼」的有/无；截图1第二位置曾显示绿色输入法图标。
不能依赖图标字形，不能补画或缓存上一状态。

UIA在下一次出现时给这些按钮分配了新的RuntimeId。
这否定了“保存旧UIA按钮身份并永久操作”的方案，但**仍不能区分原生XAML对象销毁、回收或Peer重建**。
两个按钮在UIA中都直接归于InputSite，真正的布局父容器没有暴露在这份树里。
因此不能对UIA父节点或整个托盘直接写固定95px；95px只是这次125%缩放的观测，不能成为配置默认常量。

VNC的3个样本、MobaXterm的4个样本中这两个位置均未消失。
这只说明本次场景未复现，不证明它们永远不会触发。实现不可使用进程名硬编码补空槽。

## 7. 隔离 XAML 实验（不是原生任务栏实验）

`WindowMarkXamlSlotLab.exe` 只在自身进程创建 Windows.UI.Xaml 元素，无可见窗口、无Explorer附加。
使用32/44 DIP两个模拟内容，复核以下规则：

- 内容Collapsed时，给内容设置Width不保留空间。
- 仍可见的两个父Grid设置MinWidth后，内容隐藏/移除仍保留76 DIP空空间；不设置背景刷。
- 空槽的IsHitTestVisible设为false，恢复显示时原模拟内容实例仍保留。
- **父Grid自身Collapsed时仍缩到0**，证明必须找到真正稳定且参与布局的祖先层。
- 关闭后恢复未设置值；另外验证原有非零local值和Binding对象的恢复，不能只写回猜测的0。

18项断言在当前开发机通过。它没有使用真实输入法、没有验证真实点击事件，命中测试这里只检查属性，
也没有验证WinUI3、Explorer原生容器、自定义控件、动画、异常退出或任何Win10机器。
API依据：[WindowsXamlManager初始化](https://learn.microsoft.com/en-us/uwp/api/windows.ui.xaml.hosting.windowsxamlmanager.initializeforcurrentthread?view=winrt-26100)、
[Measure/Arrange布局](https://learn.microsoft.com/en-us/uwp/api/windows.ui.xaml.uielement.measure?view=winrt-26100)。

下一原型的范围是**真实视觉树只读观察**：确定随内容一起消失的节点与不会消失的宿主，
只查询类型、父子关系、几何、Visibility/命中属性，不采集输入文本。
若需进程内诊断组件，必须与当前外部UIA探针明确区分，先验证线程亲和、解除订阅和异常清理；
不能把任意通用树调试工具直接装到用户Explorer上，更不能反复附加来轮询。
官方 [AdviseVisualTreeChange](https://learn.microsoft.com/en-us/windows/win32/api/xamlom/nf-xamlom-ivisualtreeservice-advisevisualtreechange)
可以通知视觉树结构变更，但属性显隐还需单独观察。此部分尚未实施到Explorer。

## 8. 用户提出的两条候选路线

### 普通透明通知图标互补

WindowMark自己的通知图标与输入法区域不是同一套项目；已有图标常驻不赋予指定另外两个项目位置/宽度的能力。
对端样本中普通NotifyItemIcon的3个控件均宽40px，原生目标为40+55=95px。
因此若新增两个相同规格的普通通知图标，预计只能补80px，还差15px；这是假设新增项目规格相同后的推算，
**尚未实际注册透明图标测试**，不把推算当作实测失败。
另有系统折叠/排序与异步增删产生中间位移的问题。用户要求WindowMark自身图标折叠后仍有效，
不能把“请保持托盘展开”作为必要使用条件。
依据：[NOTIFYICONDATA字段](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw)
未提供任意布局宽度/位置字段；[GetRect](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyicongetrect)
只查询位置。该候选不作为已完成方案。

### 借鉴PiInput的TSF语言栏项目

已只读检查本机PiInput当前源码，未修改/安装/激活PiInput：

- `platform/windows/tsf/lang_bar_item.cpp:396` 从输入服务的ITfThreadMgr取得ITfLangBarItemMgr。
- 同文件约415行注册GUID_LBI_INPUTMODE，约434行注册PiInput自己的产品项目，GetInfo指定CLSID_PiInputTextService。
- 同文件约447行和 `stable_text_service.cpp:810` 明确在Deactivate前移除项目；代码注释记录过旧项目未移除占着共享槽、
  导致下一输入法无法注册的历史问题。这是当前源码中的风险记录，不冒充本轮重现。
- 这条路是语言栏接口，不是Shell_NotifyIcon，因此不能用普通托盘图标的折叠规则直接断言其必定失败。

但注册语言栏项目不等于Windows提供了“替别的输入法保留两个槽”的协议。
微软 [AddItem文档](https://learn.microsoft.com/en-us/windows/win32/api/ctfutb/nf-ctfutb-itflangbaritemmgr-additem)
明确可能返回S_OK却忽略调用；不能以注册成功判定显示/占位成功。
文档对显示项目还有限制，而PiInput代码包含自己的产品项目，具体行为必须以目标Win10/11实测为准。
[语言栏样式](https://learn.microsoft.com/en-us/windows/win32/tsf/tf-lbi-style--constants)
中SHOWNINTRAY与SHOWNINTRAYONLY被文档标记为当前不支持，不能凭标志名称当成可用保证。
[ShowFloating](https://learn.microsoft.com/en-us/windows/win32/api/ctfutb/nf-ctfutb-itflangbarmgr-showfloating)
改变语言栏显示设置，不是独立空槽宽度接口，不用反复调用它对抗RDP的状态。

该候选若做最小原型，必须证明：不激活/切换PiInput或其他输入法、不注册新的键盘布局、不占用真正输入法返回时需要的共享项目、
WindowMark通知图标折叠不影响、消失与恢复时坐标确实零位移、关闭后清理全部临时项目。
若只能通过接管当前输入服务让它显示，则不满足用户“只占空间、不干扰输入”的目标。
当前结论：有可复用的TSF经验，但没有已验证的单消息保留空槽能力；不直接移植PiInput的两个按钮。

## 9. 独立TSF实验0.1的实际结果

用户同意后实现`WindowMarkTsfSlotTrial`，通过`CoCreateInstance(CLSID_TF_LangBarItemMgr)`
取得管理器，不激活ITfThreadMgr、不注册文本服务。两个私有GUID，`clsidService=GUID_NULL`；
透明32×32图标、空文字、禁用状态；不接管PiInput或系统INPUTMODE标识。
标志采用BTN_BUTTON与SHOWNINTRAY，后者的文档限制仍成立，不保证显示、宽度或位置。

开发端2026-10-24 17:09:57～17:10:10（UTC+8）运行：

- 基线管理器4项，注册后6项，移除后4项。
- 两个AddItem返回S_OK，GetItem可查询到；每项收到1次GetIcon、1次GetText、1次AdviseSink。
- 逆序RemoveItem均S_OK，随后GetItem均S_FALSE，正常清理确认。
- 并行18个有效UIA样本，当前前台ChatGPT.exe，可见托盘按钮始终13个，无新增/移除/坐标变化。

**结论仅限该条件：接受并请求图标，但没有观察到主任务栏分配新位置。**
当前开发端为RDP服务端会话，不替代另一台本地控制台上的mstsc聚焦复现。
没有证据证明“外部进程发送一个注册请求就能保留原生两个位置”；以下后续对端结果进一步否定本原型的有效性。
不会为强行显示而冒用活动输入法CLSID、占系统共享GUID或切换当前输入配置。

清理以微软[GetItem返回值定义](https://learn.microsoft.com/en-us/windows/win32/api/ctfutb/nf-ctfutb-itflangbaritemmgr-getitem)
中的S_FALSE确认“不存在”；E_INVALIDARG不能当作不存在。
正常移除的程序内确认不等于异常退出后系统侧恢复，更不是原生输入行为验收。
测试包自动计时移除，另提供同会话停止事件；拒绝重复实例，日志CREATE_NEW不覆盖旧记录。

### 对端实测补充（2026-10-24 19:04）

对端控制台收到67个有效样本。仅取两端都处于注册保持阶段的相邻样本，
20次转换中18次仍为±95px平衡位移，另外2次伴有其它按钮变化，不能算保护成功。
项目在注册阶段可查询，清理4→6→4正常，仍无法维持原生空间。
结论：普通独立进程私有TSF项目路线在本次两台设备上没有达成目标；不再要求用户重复该包。

## 10. 原生布局容器与安全门槛

本机XAML诊断观察确认 `NonActivatableStack` 在本地记事本→haps03 mstsc时同一对象由140缩至64 DIP，
其后代含 `LanguageImageIconContent`；这是含其它系统图标的共同祖先，不是专用IME槽。
随后一次真实诊断附加在订阅完成前引发Explorer崩溃，宽度写入0次；0.3版曾双侧禁用真实入口。
隔离原型通过29项布局及正常/控制程序被结束后恢复测试，不足以宣布原生可行。
已修正COM跨线程未封送问题，但未证明它就是崩溃根因。
证据、风险与待办见 [原生附加异常记录](../tests/IME-Tray-Guard-Xaml-Incident-20261024.md)。

## 11. 单订阅复用与真实占位结果

精确PDB确定MultitaskingView线程的GetRuntimeElement分类断言失败，不是MinWidth写入。
隔离双UI线程夹具还复现线程退出后再次Advise挂起，COM封送修正不能解决；并非相同FailFast复现。
0.4研究版每个Explorer只首次订阅一次，后续接受有期限控制租约，关闭恢复原值但保留连接，避免反复全根重放。
界面心跳、控制程序和目标端期限形成恢复路径；不依赖任何通知图标，不记录用户输入。

26300.9711实测固定NonActivatableStack为140 DIP后，32+44px原生按钮显隐时其余11个按钮坐标不变；
关闭后6个按钮恢复默认76px位移。证明该构建上父容器空宽度能实现目标几何效果。
其它系统图标增减、DPI和单图标消失仍需补测，不外推所有系统。
详见 [根因与实测](../tests/IME-Tray-Guard-Crash-Investigation.md)。
