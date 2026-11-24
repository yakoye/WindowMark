# 7.100 输入法占位开启但未生效的检查与恢复

## 原因

2026-11-24，主机 ypy（192.168.7.100）运行 0.6.5 / b35abec。配置 ime_guard.enabled=true，但没有输入法控制进程。程序目录保留 2026-11-23 的 active-session.lock：旧主程序 PID 3504、Explorer PID 24176；最后日志为 client_stop_cleanup_timeout。

Explorer 仍是同一进程，无法仅凭旧主程序退出断定布局已恢复，所以自动启动按设计保留偏好并暂停连接。托盘勾选代表已保存偏好，不能代替实际生效状态。没有通过删除未确认标记、重复附加或重启 Explorer 来绕过判断。

## 操作

- 先备份标记并只读检查：任务栏与已有 LocalRoots07 端点均可响应。
- 在实际交互会话 2 正常退出 WindowMark，通过公开 --ime-guard-enable 执行用户要求的明确重新启用，复用已有端点。
- 初始 state=2、width=0，等待原生图标显现并学习完整宽度；随后 state=3、width=76、writes=1、restore_failures=0。
- 连续两次读取共享协议确认心跳增加，不以配置值冒充生效。
- 再次正常退出：normalExit=true、markerRemoved=true，确认恢复闭环。
- 通过常驻 Explorer 不带启用参数重开：PID 42876，state=3、width=76、writes=1、restore_failures=0、show_hints=true。
- 后续原生日志 hint_visible=1、hint_width=76，图标隐藏后的提示语出现时，保留宽度仍为 76 DIP。

## 证据与边界

共享证据：share_chatgpt/WindowMark-ime-check-20261124；开发目录备份：build/evidence/remote-ime-7100-20261124。
包括 marker-before.txt、broker-before.json、active-before-restart*.json、normal-exit.json、verify-result.json、最终进程和原生日志。

本次恢复使用现有 b35abec 可执行文件，没有修改其 C++ 实现，也没有重新发布 EXE。正常重开后的自动恢复已经实机验证；没有模拟鼠标、截图、修改全局输入法设置或重启 Explorer。图标左右坐标逐项比较未在本轮重新采集。
