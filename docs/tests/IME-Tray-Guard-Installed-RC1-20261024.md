# 输入法占位保护：本机安装与 0.6.1-rc.1 验收

日期：2026-10-24。候选版本尚未正式发布，不能据此声明 Windows 10/11 全面支持。

## 代码、候选与交付位置

- 源码：`C:/Users/color/Downloads/WindowMark/WindowMark-dev`。
- 候选包：源码目录下 `dist/rc/v0.6.1-rc.1/WindowMark-v0.6.1-rc.1-win64.zip`。
- 本机归档：`C:/Users/color/Downloads/yakoye-release/WindowMark/v0.6.1-rc.1`。
- NAS 归档：`//192.168.7.100/Downloads/yakoye-release/WindowMark/v0.6.1-rc.1`。
- 安装目录：`C:/Users/color/AppData/Local/Programs/WindowMark`。
- 版本从正式版 0.6.0 递增至 0.6.1-rc.1，Windows 文件版本标记为预发布。完整包必须保留 `tools` 目录。

## 已完成的验证

- 工具测试 52 项通过，CTest 核心和自启动测试 2/2 通过。
- 集成构建已静默安装，主程序与五个附属程序/库逐文件 SHA-256 一致。
- 安装保留原设置与原自启动禁用状态，经常驻 Explorer 启动，脱离开发工具进程树。
- 红框目标采样 300 次：观察到两个相邻原生按钮的消失及恢复，两个转换点均核对 24 个稳定身份锚点，矩形未变。
- 锚点覆盖开始、搜索、任务视图、应用任务栏按钮、小三角、Everything 及其他托盘按钮；用户截图用于确认「中」「拼」，程序没有自动截图。
- 当前占位使用原生宿主最小宽度，不添加假图标，不逐个锁定 Everything 等应用的位置。

原始证据：`build/ime-tray-guard/evidence/installed-anchors-01/uia.jsonl`。
分析命令：`python -X utf8 tools/ime-tray-guard/check-anchors.py <采样文件>`。

## 稳定性与安装后记录

本轮 15 分钟观察位于 `build/ime-tray-guard/evidence/installed-soak-20261024-01`。
观察实际持续 901.76 秒，共 180 个样本：进程缺失、保护未生效、日志过期和错误样本均为 0；WindowMark 与 Explorer 的 PID 未变，租约宽度维持 140 DIP。该结果仅为本机短期观察，不能替代 8 小时耐久。
它测试的是安装后的集成构建（主程序 SHA-256 为 `B1B47D855A9E53E330A7B8424D94F1A5914AC8C754D241BDC118CD37234BF461`），不能混写成后续 rc.1 主程序的 15 分钟运行结果。

候选安装后的最终状态、文件校验、正常退出恢复与再次启用结果，记录在候选目录旁的 `INSTALL-VERIFICATION.md`；包内报告不预先宣称这些后续步骤成功。

## 未完成及适用范围

- 原生写入仅开放本机验证过的 Windows 11 26300.9711、单显示器、96 DPI；Windows 10 尚未实现原生保护。
- 当前验证复用已有 Explorer 订阅；首次附加发生过的跨 XAML 线程 FailFast 尚未完全闭环，不能把复用通过当作首次附加安全。
- 首次启用时原生输入法已经隐藏、容器宽度首次学习的边界仍需专项验证；本轮已学习宽度为 140 DIP，不能覆盖这一情形。
- 尚未完成多显示器、其他缩放、系统更新、Explorer 自然重启、8 小时耐久、托盘鼠标菜单和原生图标点击专项验收。
- 一秒级采样通过不等于逐帧零位移，也不能替代上述环境测试。
- 默认关闭；「留位小纸条」是后续设计，未在本候选中实现。

## 恢复与回退

先使用主面板「恢复系统布局」或正常退出，确认控制程序正常结束、日志写入/恢复次数相同、异常标记清除。不要强制卸载 Explorer 内驻留的 DLL，不需要重启 Explorer。

本轮更新前安装副本保留在 `build/ime-tray-guard/evidence/install-20261024-01/previous-install`，配置备份为同级 `settings-before.conf`。回退程序前应先确认原生布局已经恢复；保留用户测试期间的新配置，不自动覆盖。
