# 0.6.3 输入法占位配置与恢复修复

日期：2026-11-10。开发及发布检出：`WindowMark-release-0.6.3`，基于远端 v0.6.2；原开发目录和历史发布目录保留。

## 根因

旧版 `Pause`、失败分支和租约结束路径调用 `SetRequested(false)`，通过回调把 `ime_guard.enabled=false` 写回配置。主程序自动启用失败也会直接保存关闭。旧标记只记录 WindowMark PID，无法判断旧 Explorer 的布局是否已经随进程消失。TaskbarCreated 只暂停，没有重新连接。

## 修复与验收设计

- [x] 只有显式恢复/关闭改变已保存启用偏好，失败保留设置。
- [x] 启动先等待稳定，任务栏重建及进程身份变化触发恢复。
- [x] 保留进程句柄并记录 PID/创建时间；旧目标死亡后不再要求旧进程返回恢复确认。
- [x] 标记兼容迁移；同一仍存活目标的未确认标记保守暂停。
- [x] UI 勾选显示偏好，状态文字显示实际生效情况。
- [x] 七项生命周期测试通过；只终止本测试创建的屏幕外 XAML 宿主，不操作日常 Explorer。

证据：`build/evidence/v0.6.3-lifecycle-02/result.log` 七项 PASS；最终启动等待入口复测在 `v0.6.3-lifecycle-03` 同样七项 PASS。第一轮测试使用相对证据目录，控制程序工作目录切换后找不到日志路径，退出码 23；已将测试路径转为绝对路径后复测，保留失败原件。

新增用例覆盖：开机未就绪时保留配置、TaskbarCreated 后恢复、目标进程死亡后连接新目标、主程序重开、死亡目标的旧标记、存活目标未确认时保持偏好且不重试、旧格式标记与手动关闭取消恢复。

普通程序重开通过新建后端实例验证；真正整机重启没有自动执行。Windows 10、高 DPI、多屏和新系统内部接口的范围不因本次状态机修复扩大。

## 最终结果与本机安装

- Release 构建通过，产品及文件版本 0.6.3，IsPreRelease=false。
- 核心 CTest 2/2、Python 52/52（无跳过）、新增生命周期七项、既有提示语/透明模式/动态开关/恢复四组均通过。
- 四组回归证据：`build/evidence/v0.6.3-regression-01/summary.json`。
- 安装前运行版本为 0.6.1-rc.3，安装后六个程序组件与构建产物 SHA-256 一致。
- 本机先显式启用，PID 23788 的保护 state=3、width=140、show_hints=true；正常退出确认恢复并清理标记。
- 随后通过常驻 Explorer 不带参数启动 PID 36736，父进程 16620，自动恢复到 state=3、width=140、show_hints=true、restore_failures=0。证明真实安装版在程序重开后保留并自动应用设置。
- 证据：`build/evidence/installed-0.6.3/before-restart.json`、`normal-exit.json`、`after-restart.json` 和 `hashes.json`。
- 安装器启动子程序稍晚于第一条退出请求，首次等待未结束；再次正常请求退出后完成安装验证，没有强杀用户进程或重启 Explorer。

本轮已实际重开 WindowMark；Explorer 退出重建路径由自建 XAML 宿主真实退出/新进程启动验证，未强制重启用户正在使用的 Explorer。
