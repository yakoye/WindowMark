# WindowMark 输入法占位预研：只读采集包 0.2

这是诊断工具，不是已完成的占位保护。**运行后图标仍可能移动，这是预期的原始行为。**
它不注入 Explorer、不改任务栏/输入法设置、不添加图标、不截屏、不抢焦点，不需要管理员权限。
Windows 10/11 x64 为采证目标；目前仅在开发机 Windows build 26300 的 RDP 会话运行过。
旧版 Win10 未验证，若程序无法运行请记录错误，不要修改系统配置来迁就工具。

## 给另一台电脑：现在就能做的测试

1. 从本地 `C:\Users\color\Downloads\share_chatgpt\WindowMark-IME-Tray-Guard` 打开最新的
   `probe-0.2-*` 文件夹。请在**本地控制台**测试，不要进入远程电脑后采它的任务栏。
2. 打开记事本和要测试的客户端（RDP、VNC Viewer 或 MobaXterm），先用窗口模式。
   不需要输入密码给本工具，不必更改现有连接、语言设置或任务栏设置。
3. 双击 `capture-rdp.bat`（或对应的 `capture-vnc.bat`、`capture-mobaxterm.bat`）。
   双击通用 `capture.bat` 也可，但请在反馈注明客户端。
4. 有 **8 秒**切回记事本。采集开始后：记事本约 10 秒 → 客户端约 10 秒 → 记事本约 10 秒，
   重复一轮，总计约 60 秒。鼠标和切换由你自己操作，工具不会操控。
5. 记下实际看到的「中」「拼」是否消失、是否恢复，其他图标是否移动。
   如方便，手动提供正常/消失两张任务栏截图；不需要截整个桌面。
6. 完成后窗口显示 `Capture exit code: 0`，按任意键关闭。
   `results` 中产生 `.jsonl`、`.run.json` 和两份日志，**全部保留**。
   已在共享目录运行就不必再拷贝，告诉本会话目录及测试完成即可。
7. 复制 `反馈模板.md`，按设备/测试日期另存填写。保留原模板，不覆盖别人的结果。

建议依次测试 RDP、VNC Viewer、MobaXterm（没有安装的写“未执行”即可，不要求另装）。
若可用另一台 Windows 10 电脑，同样运行一遍；Windows 11 的结果不能代替 Win10 验证。
当前阶段不要求重启 Explorer、断开正在工作的 RDP、改变 DPI 或注销电脑。

## 失败时

- 返回 124：采集子进程超时，启动器已尝试停止**自己的子进程**。保留所有文件，告诉开发端。
- 返回其他非零值或找不到 `.run.json`：运行未成功，保留日志/错误原文，不标为通过。
- 程序不启动或安全软件拦截：记录提示，不关闭安全软件、不添加白名单；向开发端反馈。
- 想提前结束：优先在启动器窗口按 Ctrl+C；它会尝试清理自己的子进程。
  强行关闭控制台窗口不保证 finally 运行，必要时只结束 `WindowMarkImeTrayProbe.exe`。
  工具没有系统布局写入，停止后无需重启 Explorer 或恢复设置。

## 数据范围与限制

记录系统构建/Explorer 文件版本、会话类型、屏幕/DPI、前台 EXE 文件名、任务栏 HWND/UIA
类名、控件 ID、矩形、可见性、RuntimeId 与错误码。不记录输入内容、剪贴板、窗口标题或截图。
读取部分托盘按钮的可访问名称做 IME 分类，只落盘固定类别词，原始名称（可能含 SSID 等）丢弃。
分类是线索，不是确认了两个原生元素。任务栏按钮路径不采集。

默认约每秒一次，扫描本身也耗时；不是逐帧录像，无法凭采样证明“始终零位移”。
UIA 不是 XAML 视觉树，控件不在 UIA 中不代表它已销毁。
不能把没有识别到 IME、没有观察到移动、或 EXE 返回 0 当成保护有效。
返回 0 仅表示进程完成基本采集，树错误/截断仍须检查 JSON。

## 开发端分析（对端不需要安装 Python）

```powershell
python -X utf8 analyze.py results/某次采集.jsonl
```

输出逐文件诊断；不同机器/会话的文件绝不合并当作同一测量。
`max_rect_delta_px` 是同一段基线与后续样本间、未标记为 IME 的可见按钮矩形最大差值，
其中仍可能包含未识别的 IME 按钮。节点消失单独记录，不算“位移为零”。
存在读取问题、环境变化、长采样间隙或不足两个可比较样本时分析器返回 2。

## 构建与测试（仅开发端）

```powershell
cmake -S tools/ime-tray-guard -B build/ime-tray-guard -A x64
cmake --build build/ime-tray-guard --config Release --target WindowMarkImeTrayProbe WindowMarkImeProbeFixture
$env:IME_PROBE_EXE = (Resolve-Path build/ime-tray-guard/Release/WindowMarkImeTrayProbe.exe).Path
python -X utf8 -m unittest discover -s tools/ime-tray-guard -p 'test_*.py' -v
./tools/ime-tray-guard/test-launcher.ps1 -ProbeExe $env:IME_PROBE_EXE -FixtureExe (Resolve-Path build/ime-tray-guard/Release/WindowMarkImeProbeFixture.exe).Path
```

`ProbeFixture` 是测试故障/挂起的夹具，禁止作为采集工具交付。
本包没有安装/自启/写入注册表逻辑；保留结果后删除包目录即可移除工具。
