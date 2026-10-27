# WindowMark 0.6.1-rc.2 安装与交付核验

日期：2026-10-24。此记录在候选 ZIP 冻结后补充，与 ZIP 一同归档，不覆盖 rc.1，不公开发布。

## 当前状态

- 已安装到 `C:/Users/color/AppData/Local/Programs/WindowMark/WindowMark.exe`。
- 主程序 PID 18540，父进程为常驻 Explorer PID 38696，脱离开发工具启动。
- 最后复核面板显示「原生占位保护已生效：140 DIP」；最终保留启用。新安装的默认配置仍为关闭。
- 最终租约日志 `18540-872565343.jsonl`：Reserved，140 DIP，writes=1、errors=0、subscription_active=0。
- 主程序、ClipKeeper、诊断、检查器、控制程序及 DLL 六文件与构建产物 SHA-256 全部相同。
- 原生受控试验加载的 DLL 与安装 DLL 哈希相同，安装版复用的是新局部根组件。

## 已完成验收

1. 旧版实际鼠标点击会创建无 owner 的确认框；新版本确认框归属面板，重复弹出受限。安装版实点复选框后确认启用，实际状态进入 Reserved。
2. 删除全局视觉树订阅；新路径只查询当前 XAML 线程的根。只在精确匹配的加载映像上调用，未知映像拒绝，不回退旧路径。
3. 自建双线程宿主动态布局、副线程退出、三次占位与恢复通过；1 次写入/1 次恢复的持续租约、心跳停止、界面进程死亡三项通过。
4. 旧路径曾卡住的「副线程退出后再次附加」场景：新路径两次均正常退出，宿主无异常。
5. 本机 haps03 受控显隐：100 样本中的一次两个按钮隐藏事件为 -76px，24 个既有锚点均不移动，包含小三角、Everything、开始及应用按钮。90 秒租约结束确认 1 写 1 恢复、0 错误。
6. 安装版连续观察 181.43 秒、37 样本，缺失进程/保护失效/日志过期/错误均为 0；随后主动关闭确认原值恢复，最后重新启用。
7. 52 项 Python 测试、CTest 核心和自启动两项通过；最终主程序文件属性和面板版本均为 0.6.1-rc.2。

## 不能宣称完成的部分

- 安装后另一次 300 样本没有显隐事件，几何分析明确不通过；不能算新的显隐成功。haps03 已最小化，工具检测到外部输入后拒绝恢复窗口，未绕开保护。
- 原始转储缺少对象堆，无法还原具体失败根对象；已移除其全局回放调用路径，不等于还原了全部 Windows 内部根因。
- 新路径依赖限定映像的私有 ABI。全新 Explorer 进程启动、系统更新、多屏/DPI、8 小时耐久、托盘菜单鼠标专项、原生图标菜单命中仍未完成；本次未主动重启 Explorer。
- Windows 10 按用户最新要求暂停。

## 目录和文件

- 开发代码：`C:/Users/color/Downloads/WindowMark/WindowMark-dev`。
- 候选：`dist/rc/v0.6.1-rc.2/WindowMark-v0.6.1-rc.2-win64.zip`。
- 本地归档：`C:/Users/color/Downloads/yakoye-release/WindowMark/v0.6.1-rc.2`。
- NAS 归档：`//192.168.7.100/Downloads/yakoye-release/WindowMark/v0.6.1-rc.2`。
- 两处均放 ZIP、完整解压的 `WindowMark/` 目录和本核验记录。运行 `WindowMark/WindowMark.exe`，保留 `tools/`，不可只移动裸 EXE。

ZIP：1,714,200 字节、27 个文件。

| 文件 | SHA-256 |
| --- | --- |
| WindowMark-v0.6.1-rc.2-win64.zip | `096A06840EA3346DEC06BBF9E026A21BA7DE849C56A4E8AE34677D68FE7F33C5` |
| WindowMark.exe | `984F855679161B0344E0CA88321052A6904FACBFCDB52FE7DA2E5BE55B087F15` |
| WindowMarkXamlContainerTap.dll | `0A400B9BB829223CA4E4EC45C4F05A12F6FA39B13D8F6619975F443C4C55524D` |

没有创建标签、推送代码或 GitHub Release。候选包保留验收边界，不能称为稳定正式版。
