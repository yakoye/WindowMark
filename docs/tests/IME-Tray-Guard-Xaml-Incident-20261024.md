# 原生容器预研与附加异常记录

日期：2026-10-24，时间均为北京时间，另标UTC者除外。本文保留19:27异常和0.3隔离版的历史结论。
后续精确符号定位FailFast；0.4单订阅复用后，本机原生占位/恢复实测有效，仍非完整验收。
最新状态见 [根因与本机试验](IME-Tray-Guard-Crash-Investigation.md)；下文未取得转储/入口禁用是当时状态。

## 目标和实际操作

研究原生输入法父容器能否保留原宽度，并恢复原始未设置值、局部值或绑定。
按用户更正，仅切换已经打开的 `haps03-192.168.80.158` 远程桌面窗口和本地记事本。
未读取RDP文件中的连接信息、未输入凭据、未重连或关闭远程会话；未自动截图。
不是修改远程桌面内部任务栏，而是观察当前桌面的Explorer。

诊断接口需要加载自写TAP DLL到目标进程。这与外部UIA只读采集不同：即使属性写入0次，附加仍有进程稳定性风险。
本轮没有安装产品、注册输入服务、改输入法设置、主动重启Explorer或输入法进程。

## 已获得的原生结构证据

证据目录：`build/ime-tray-guard/evidence/container-native-20261024-192327`。

- 诊断观察正常结束；宽度写入0次、读取错误0、溢出0、取消订阅返回S_OK。
- UIA 65个有效样本，无读取问题；本地记事本38个样本有13个按钮，mstsc的27个样本有11个。
- 样本38（UTC 11:24:07.935）切入mstsc后移除76px，6个既有按钮平移+76px。
- XAML `SystemTray.Stack`、名称 `NonActivatableStack` 的同一对象，宽度从140变为64 DIP。
- `LanguageImageIconContent` 位于其 `Content → IconStack → ItemsPresenter → StackPanel → ContentPresenter → SystemTrayIcon` 后代链。
- 100% DPI下此次缩减76 DIP；另一台125% DPI采集为95px，不混用像素与DIP。

候选是包含输入法及其它系统图标的共同祖先，不是已证明只含输入法的专用容器。
单凭设定其MinWidth，不能保证内部对齐、其它图标位置、空白命中和原生菜单不变。

## 异常时间线

证据目录：`build/ime-tray-guard/evidence/container-width-native-20261024-01`。

| 时间 | 证据 |
| --- | --- |
| 19:27:36.361 | 第二次实验启动，InitializeXamlDiagnosticsEx返回S_OK |
| 19:27:36.463 | Attached状态，UI线程26832；写入0次、节点为空、采样tick为0，尚未完成订阅 |
| 19:27:37 | Windows事件1000：Explorer.EXE PID20748崩溃，故障模块Windows.UI.Xaml.dll |
| 随后 | 控制程序记录target_exited，停止本轮，不再进行真实附加或窗口切换 |
| 19:27:46 | Windows自行启动Explorer PID38696；不是本工具执行的重启 |

事件详情：模块版本10.0.26100.9711，异常0xc000027b，偏移0x667f；
报告ID `0f6d48a4-2164-4ad5-b72f-2eb7969f2004`。WER事件1001含APPCRASH、StackHash12_55a、P7=8000ffff。
事件文本保存在上述目录的 `crash-events.json`。
受保护的WER归档读取被拒绝，未绕过权限；没有取得可用于完整调用栈分析的转储。

**没有发生MinWidth写入，不能将这次崩溃归因于宽度值，也不能把返回S_OK当成安全附加成功。**
先前观察会话与随后宽度会话之间存在重复附加，但在自建宿主中重复该顺序没有复现；重复附加是待查条件，不是已确定根因。

## 修正与限制

代码审查发现诊断服务COM接口从STA直接交给MTA工作线程使用，缺少封送。
现改为 `CoMarshalInterThreadInterfaceInStream` / `CoGetInterfaceAndReleaseStream`，订阅和退订使用工作线程代理。
恢复计时从SetSite启动，不再等待订阅完成后才启动；但不能保证可以安全取消一个永久阻塞的COM调用。
封送修正后的隔离恢复测试通过，**没有再次试验真实Explorer，不能宣称崩溃已修复**。

0.3版保护措施：

- 控制程序在创建证据、加载目标DLL之前拒绝两个真实任务栏命令。
- DLL拒绝非夹具请求，并核对目标进程文件名为 `WindowMarkXamlContainerFixture.exe`。
- 只允许自身夹具进行显式、限时的布局事务；正常退出和控制程序被结束后都尝试恢复。
- 旧0.1/0.2本地实验控制程序改为 `.exe.disabled` 保留证据，未删除；未发到共享目录。
- 取消订阅不等于卸载诊断DLL，不强行FreeLibrary。隔离宿主退出才结束该DLL的进程寿命。

## 本轮通过的隔离验证

最终包：`dist/test/container-lab-0.3-20261024`。
实际入口测试证据：`results/lab-20261024-193619-193-606858d5`。

| 项目 | 结果与边界 |
| --- | --- |
| Release构建 | MSVC `/W4 /WX`通过，没有关闭告警 |
| Python | 43项通过、无跳过；含5项新控制程序门禁测试 |
| XAML布局与事务 | 29项通过，含unset、数值、绑定恢复、折叠/非法宽度拒绝 |
| 正常退出 | 内容隐藏时76 DIP保留，随后恢复0/unset；内容回来后自然宽度76 DIP |
| 控制程序被结束 | 只结束实验控制程序；宿主检测到退出后恢复0/unset，原内容恢复76 DIP |
| 旧版双附加隔离复现 | 未复现Explorer崩溃，不是对原生安全性的证明 |

早期夹具只得到默认根节点、写入0次的运行保留为失败/无效验证；没有把退出0算成布局通过。
使用IVisualTreeServiceCallback2后才观测到夹具的XAML Island内容并完成事务。
初次编译错误及一次运行中EXE锁定导致的链接错误均已修正后重编译，未掩盖为零错误。

## 可行性结论与下一步

原生候选容器已经找到；隔离环境可以保留和恢复宽度。**当前附加路径存在实际稳定性风险，不具备正式开发集成门槛。**
暂不让另一台电脑重复危险附加；共享目录仅提供夹具测试包和报告，不提供真实功能开关。
下一阶段需要围绕附加、订阅、线程及旧会话残留获取可复核根因证据，先证明安全生命周期，再考虑真实宽度写入。
Windows 10、原生点击、空白命中、多屏、更新兼容、Explorer重建及八小时耐久均未通过；本轮不发布。

## 接口依据

- [InitializeXamlDiagnosticsEx](https://learn.microsoft.com/en-us/windows/win32/api/xamlom/nf-xamlom-initializexamldiagnosticsex)：目标进程诊断DLL初始化。
- [AdviseVisualTreeChange](https://learn.microsoft.com/en-us/windows/win32/api/xamlom/nf-xamlom-ivisualtreeservice-advisevisualtreechange)：视觉树订阅。
- [CoMarshalInterThreadInterfaceInStream](https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-comarshalinterthreadinterfaceinstream)：跨线程COM接口封送。

实现基于SDK接口自写；上游研究只用于确认结构/API，不复制GPL实现。
