# 输入法占位保护：主程序集成实测

日期：2026-10-24。结论：主程序已集成并通过下列限定测试，尚不满足 Windows 10/11 全面稳定发布条件。

## 实测环境

Windows 11 26300.9711，Explorer 10.0.26100.9711；当前 RDP 会话 3，单屏 1680×950、96 DPI。
触发程序为已连接的 haps03 / mstsc 与本地记事本；未读取 RDP 文件或凭据，未操作远程应用内容，未截图。
Explorer PID 38696，自 19:27:46 起未变化；本轮没有主动重启 Explorer。

## 通过项目

| 项目 | 证据与结果 |
| --- | --- |
| 构建 | 主工程 Release，MSVC `/W4 /WX`，命令退出 0，无编译警告或错误 |
| 配置与独立性 | 核心测试含默认关闭、true/false 读写、非法值、旧配置兼容、构建白名单、真实生效及恢复判据；IME 开关不刷新窗口后端 |
| 原有回归 | CTest 的 core/autostart 两套全部通过 |
| 原型回归 | Python 45 项全部通过；没有跳过配置好的 EXE 测试 |
| 夹具故障 | 持续 40 秒越过初始期限、UI 心跳失联、UI 进程死亡：均 1 次写入、1 次恢复、0 次恢复失败 |
| 主面板默认关闭 | 启动无原生控制进程、无运行目录；只读兼容检查不连接 Explorer |
| 主面板交互 | 实测检查、启用确认、等待生效、140 DIP 实际生效、恢复中、恢复完成；Alt+I/R/K 可操作 |
| 原生显隐 | 240 个有效样本，5 次可比较显隐转换；两个 32+44px 按钮离开/返回可见集合时，其余 10 个按钮位移为 0px |
| 一键恢复 | 主程序按钮触发，日志 Closed、writes=1、restores=1、restore_failures=0；配置 false，恢复标记清除 |
| 真实进程异常 | 只结束测试主程序 PID 7856；控制日志确认 1 次恢复、0 次失败；Explorer 未退出 |
| 异常时布局 | 180 个有效样本：保护时 10 个既有按钮不动；主程序被结束后，保持 mstsc 前台，5 个按钮恢复默认 +76px 位移、5 个不动 |
| 异常重启 | 标记保留；再次启动拒绝自动连接，界面显示恢复未确认并保存 enabled=false |
| 安装目录保护 | 原生控制组件从独立运行目录启动，不直接加载安装目录 DLL；未覆盖当前安装版 |

前一研究阶段有 11 个既有按钮，本轮有 10 个，属于当前采样集合不同；不把两次统计混写。
240 样本期间录到两次完整来回及第三次隐藏，第三次返回发生在采样结束后，不计入已捕获转换数。
原生持续运行观察约 3 分钟，**不是** 8 小时耐久测试。

## 可复核证据

### 最终包复测

从完整 ZIP 解压运行的最终主程序 SHA-256：
`B1B47D855A9E53E330A7B8424D94F1A5914AC8C754D241BDC118CD37234BF461`。
21:45 再测默认关闭无控制进程、手动启用显示 140 DIP、生效后恢复：均通过。
最终日志 `dist/test/ime-integrated-final-20261024/ImeTrayGuard/36084-866827375.jsonl` 为
`reuse_subscription`，最终 Closed、writes=1、restores=1、restore_failures=0，恢复标记已清除。
补入本段后仅重新打包文档，不重编译主程序；ZIP 内二进制逐项 hash 验证。

### 原始数据

- `build/ime-tray-guard/evidence/integration-client-lease-01/summary.json`：三种租约测试。
- `build/ime-tray-guard/evidence/integrated-native-01/uia.jsonl`：240 样本。
- `dist/test/ime-integrated-20261024-03/ImeTrayGuard/2936-866168921.jsonl`：原生启用及按钮恢复。
- `build/ime-tray-guard/evidence/integrated-owner-exit-01/uia.jsonl`：180 样本。
- `dist/test/ime-integrated-20261024-04/ImeTrayGuard/7856-866472937.jsonl`：异常恢复。
- 两次原生会话首条均为 `reuse_subscription`，没有再次全根 Advise。

采集器历史字段 `protection_active=false` 只描述采集器自身不施加保护，不能作为其他进程状态。此次新增 `protection_state_scope=collector_only` 及明确控制台文字，分析器也不再声称数据一定采于保护关闭状态。实际生效依据控制日志与 UIA 几何联合判断。

## 修复过的测试发现

- Windows 自定义模态窗口未自动响应新增助记键：增加显式分发后实测通过。
- 初次以 Hidden 启动导致主面板不可见：测试改为常驻 Explorer 的 ShellExecute，父进程确认属于 Explorer，不改生产可见性逻辑。
- 新配置若走全部 Settings 更新会刷新书签/边框：增加独立配置 setter，并以 mock 后端更新次数回归。
- 一次测试启动路径拼写错误只产生“找不到文件”提示，关闭提示后改为先验证路径；未触发原生附加。
- 恢复入口在无活动进程但有异常标记时不再显示普通关闭，避免掩盖未确认恢复。

## 尚未通过的发布门槛

- Windows 10 原生容器识别、写入与恢复；已提供专用只读工具，等待真实 Windows 10 证据。
- 首次全根诊断附加的跨线程 FailFast 完整根因；本轮复用订阅，不是重启后首次附加验收。
- Explorer 自然重启、Windows 更新、多屏、非 100% DPI、锁屏/休眠真实场景及 8 小时运行。
- 托盘实体图标的鼠标交互、原生输入法左/右键、空白占位命中、仅单个原生图标消失。
- 安装/卸载的用户交互端到端；当前只更新构建、打包及安装复制清单。

以上缺口不以配置开关、模拟夹具或编译成功冒充通过。本次产物是 WindowMark 主程序集成测试包，不是独立预览 GUI，也不是正式稳定发布。
