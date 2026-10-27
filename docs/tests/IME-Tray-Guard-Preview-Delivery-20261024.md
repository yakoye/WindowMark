# 输入法占位保护研究预览交付记录

日期：2026-10-24。产物：`ime-guard-preview-0.4-r3-20261024`，不是WindowMark正式版本升级。

## 交付

- 本地：`C:/Users/color/Downloads/yakoye-release/WindowMark/preview/ime-guard-preview-0.4-r3-20261024`
- NAS：`//192.168.7.100/Downloads/yakoye-release/WindowMark/preview/ime-guard-preview-0.4-r3-20261024`
- 协作：`//192.168.7.100/Downloads/share_chatgpt/WindowMark-IME-Tray-Guard/ime-guard-preview-0.4-r3-20261024`
- 对端本地协作路径：`C:/Users/color/Downloads/share_chatgpt/WindowMark-IME-Tray-Guard/ime-guard-preview-0.4-r3-20261024`

入口`WindowMarkImeTrayGuard.exe`，须保留同目录控制程序与DLL。包含中文说明、计划、实测报告、ASCII双击BAT、SHA256SUMS及ZIP。
三处文件与ZIP逐一比对一致。GUI EXE SHA-256：
`E3D7D11C10C9AEBF655F40E3F278A3BCDD740A0708D67C1F840922317404093B`。

## 最终修订验证

最终r3已在本机运行：默认关闭，Alt+E进入140 DIP保护，Alt+R恢复，再次启用使用reuse_subscription，
在生效状态下关闭窗口，日志分别确认两次会话均1写1恢复、0错误。
运行记录：`%LOCALAPPDATA%/WindowMark/ImeTrayGuardPreview/17688-862711890.jsonl`及同PID后续会话。
实验结束无预览界面/控制程序残留，Explorer仍为PID38696、启动时间19:27:46、响应正常；未主动重启。
诊断连接仍在Explorer内但无宽度覆盖，不将其误称为卸载。

本机真实异常退出、320个界面采样、180个原型采样、45项Python、29项布局和两个既有CTest结果见根因与实测报告。
未替换当前WindowMark安装、未改用户settings.conf、未推送或发布GitHub Release。

## 仍需完成

正式主面板/托盘/配置集成、Win10真实实现与验证、原生菜单/空白命中、单个图标显隐、
100次切换、VNC/MobaXterm、DPI/多屏、Explorer重建及8小时耐久。
对端可先验证相同构建下的菜单、显隐与恢复；首次附加风险须知见README，未知构建拒绝，不绕过门禁。
