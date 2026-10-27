# 0.6.1-rc.1 安装后核验

日期：2026-10-24。状态：已安装本机、已双目录归档、未正式发布。

## 最终交付

- 已安装：`C:/Users/color/AppData/Local/Programs/WindowMark/WindowMark.exe`。
- 主程序、主面板、文件属性和卸载项版本：`0.6.1-rc.1`；文件资源标记为预发布。
- 完整包：`WindowMark-v0.6.1-rc.1-win64.zip`，1,707,494 字节，26 个文件。
- ZIP SHA-256：`142E229B5176226DD6C3C9C622AD1A454550992539E699F25DA1153288A6E4DB`。
- 主 EXE SHA-256：`1273BA48D0913FA6A274AA9324030B86FCB604A332603D1BCA66F3DB05979A57`。
- 两处版本目录均保留完整 ZIP 和 `WindowMark` 解压目录；不能只拷贝主 EXE 丢掉 `tools`。
- 最终安装版经常驻 Explorer 启动；占位保护恢复为关闭，面板显示「已关闭；不改变原生输入法及任务栏布局」。自启动保持关闭。
- 临时配置诊断构建未放入交付目录，诊断代码已撤回；后续开发构建的时间戳不作为本候选包身份。

## 安装与测试证据

1. 安装器退出 0；安装目录六个二进制与候选解压目录逐文件 SHA-256 一致。卸载注册表 `DisplayVersion` 为 `0.6.1-rc.1`。
2. 前一集成构建已完成 901.76 秒、180 样本观察：进程缺失、失效、日志过期、错误均为 0。此项不是 rc.1 主程序的 15 分钟耐久报告。
3. rc.1 启用后原生容器为 140 DIP，未添加替代图标。第一段采样 60 次，两个显隐转换点的 25 个稳定身份锚点全部保持原坐标。
4. 第二段采样 40 次，识别到 13 个两按钮增减事件，合计宽度均为 ±76 像素；25 个稳定身份锚点未移动，覆盖小三角、Everything、开始/搜索/任务视图、应用按钮及其他托盘按钮。事件数是采样结果，不等于脚本主动切换次数。
5. rc.1 的恢复日志为 state=5、writes=1、restores=1、restore_failures=0、errors=0；异常恢复标记清除。Explorer PID 38696 未变。
6. 52 项工具测试、CTest 2/2 通过；根工程编译通过。没有执行公共发布、推送或打 tag。

原始证据位于源码目录 `build/ime-tray-guard/evidence`：

- `installed-soak-20261024-01/summary.json`
- `installed-anchors-01/uia.jsonl`
- `rc1-anchors-01/analysis.json`
- `rc1-anchors-02/analysis.json`
- `install-rc1-20261024/installed-files.json`

## 配置专项核对及过程异常

外部 PowerShell/Node 读取的配置文件仍显示旧内容，而临时诊断构建在进程内保存成功，并回读启用=true、7170 字节；正常退出后再启动，实际自动恢复启用。关闭时回读=false、7171 字节，随后启动最终安装版确认为关闭。由此验证了实际开关的跨启动行为；外部读取与程序视图差异原因尚未确认，不能声称已经修复一个配置缺陷。

诊断输出保留在 `install-rc1-20261024/config-roundtrip-diagnostic.txt`；未复制用户配置到共享目录。安装校验脚本曾因只读注册表句柄无法写回 StartupApproved 而中止，随后使用可写操作恢复禁用状态并重新核对；未新增 Run 项。临时诊断构建第一次重链接时因自身 EXE 仍运行而失败，正常退出、确认原生布局恢复后重建通过，未结束 Explorer。

## 尚未通过的发布门槛

只开放 Windows 11 26300.9711、单屏、96 DPI。Windows 10 原生保护尚未实现；其他构建、多屏和缩放、首次附加的 FailFast 根因闭环、原生点击与空白命中专项、8 小时耐久仍待完成。当前证据复用已有 Explorer 订阅，不能代表首次附加安全。首次在输入法隐藏时学习宽度的情形也尚未覆盖。

「留位小纸条」已记录设计，未在此候选中实现；默认保持透明留白。

## 另一台电脑协作

新版只读工具放在共享目录 `//192.168.7.100/Downloads/share_chatgpt/WindowMark-IME-Tray-Guard/anchors-readonly-20261024-rc1`。对端本地对应 `C:/Users/color/Downloads/share_chatgpt/WindowMark-IME-Tray-Guard/anchors-readonly-20261024-rc1`。

双击 ASCII 的 `collect-anchors.bat`，按 README 在本地编辑器和已连接远程客户端间切换，返回 results。工具不注入、不改系统设置、不截图；仍需要真正 Windows 10 的采样，Windows 11 不能代替。
