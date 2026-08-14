# 开发计划

按优先级排列。每项都写明**为什么需要**，避免以后回头看只剩一句功能名。

## 窗口边框

### 1. window_rules：按窗口排除边框

tacky-borders 默认排除了这几类，不排除会给不该加边框的系统窗口画上边框：

```yaml
- match: Class    name: "Windows.UI.Core.CoreWindow"        # UWP 宿主
- match: Class    name: "XamlExplorerHostIslandWindow"      # 资源管理器的 XAML 岛
- match: Title    name: "Picture-in-Picture"                # 画中画
- match: Process  name: "zebar" / "seelen-ui" / ...         # 其他桌面挂件
```

需要支持 Class / Title / Process 三种匹配，以及 Equals / Contains / Regex 三种策略。
配置格式待定：现有 `settings.conf` 是扁平的 key=value，表达规则列表比较勉强，
可能需要一个单独的规则文件或改用分节格式。

**已经硬编码的部分**：`WinUtil.cpp` 里的 `kExcludedClasses` 目前排除九个类，包括
`Windows.UI.Core.CoreWindow`（输入法候选框和键盘布局浮出）和 `XamlExplorerHostIslandWindow`。
这一项要做的是把这个列表变成用户可配置的，而不是从零开始——遇到新的系统窗口时，用
`list_tracked.bat -Watch 15` 拿到类名，先加进硬编码列表也能立刻解决。

### 2. initialize_delay / unminimize_delay

窗口打开和从最小化恢复时有系统动画，边框如果立刻出现会贴在动画中途的位置上，
看起来是错位的。tacky-borders 的默认值是 200ms / 150ms。

### 3. follow_native_border

Windows 自己会在某些窗口状态下隐藏原生边框（例如无边框全屏）。跟随这一行为，
避免在全屏视频/游戏上画出一圈边框。

### 4. 工作集内存

当前 12 个窗口时私有内存 55MB（低于 tacky-borders 的 73MB），但工作集 122MB
（高于它的 67MB）。差异来自呈现方式：

- 我们用 `UpdateLayeredWindow`，系统会为每个分层窗口保留一份完整位图；
- tacky-borders 用 `SetLayeredWindowAttributes` + 常规绘制，没有这份系统侧副本。

任务管理器显示的是工作集，所以这个差距是用户可见的。要抹平需要改用
DirectComposition 或 DXGI swap chain 呈现，工作量不小，且当前跟手与私有内存都已达标，
因此排在功能项之后。

### 5. 无激活的纯 z 序变化不会被观察到

如果某个应用把自己的窗口抬到上层但**不夺取焦点**（`SetWindowPos(HWND_TOP, SWP_NOACTIVATE)`），
我们收不到任何事件，边框会暂时留在原来的深度，直到下一个无关的窗口事件顺带把它修好。

已经找过便宜的补救办法，没有：`EVENT_OBJECT_REORDER` 对顶级窗口不触发（实测静置 10 秒
0 次，程序化抬升 6 个窗口也是 0 次，16 次全部来自子对象）。剩下的办法只有定时轮询，
而这违背「对系统影响极小」这条硬要求，所以不做。

实际影响很小：真实操作里的层级变化都伴随焦点切换，而 `EVENT_SYSTEM_FOREGROUND` 会触发
一次全量 `Apply`，把**所有**边框重新锚定。真要处理，得先找到一个不用轮询的信号。

v0.3.6 补充：曾尝试让 `SyncZOrder` 只在激活状态或可见性变化时运行，但实测边框层级会在
未收到对应事件时漂移，之后没有机会自动修复，因此这项优化已经撤销。当前仍会在移动路径中
校正层级；真正解决拖动卡顿的是移除书签窗口的跨进程 owner，详见 `CHANGELOG.md`。

### 6. 系统强调色 / 主题感知

tacky-borders 支持 `accent` 关键字和 dark/light 分别配色，随系统主题切换。

## 书签

### 7. 重命名的持久化

目前自定义名称只在本次运行有效，因为通用窗口没有可靠的跨会话标识。
如果要持久化，需要能稳定识别"同一个窗口"的依据（例如某些应用可以从命令行、
工作区文件或标题模式推断），这需要按应用做适配，不能在 Core 里靠标题猜。

### 8. 悬停预览的实机验证

层级已改为显示时置顶，但预览是否真的出现、DWM 缩略图是否正常，尚未实测。

## 架构

### 9. 每功能一线程（加入第三个功能时再做）

现在书签和边框都跑在 UI 线程上。

原本这里写着「实测表明这不是瓶颈（46/80 vs 46/80）」——那个结论作废了，它用的是 v0.3.5
那套信噪比过低的指标。v0.3.6 用新指标测出来：单开书签 24ms、单开边框 30ms、两个都开
38ms（基线 16ms），**两个功能之间存在明显的相互放大**，不是简单相加。机制也清楚了：
每次 `SetWindowPos` 都要拿桌面全局窗口锁，而被拖动的程序自己移动时需要同一把锁。

即便如此，现在拆线程仍然不划算：**多线程并不能减少 SetWindowPos 的总次数**，锁是全局的，
换个线程去抢还是一样抢。真要继续压这 22ms，方向是减少调用次数或改用 DirectComposition
呈现，不是加线程。线程化的理由仍然只有「加入第三、第四个功能之后 UI 线程累积负载过高」。

但如果将来加入第三、第四个功能，UI 线程的累积负载会成为问题。届时的模式应该是：

- 主线程：唯一的 WinEvent hook + Coordinator（窗口状态的单一真相源）
- 每个功能：自己的线程、自己的窗口、自己的消息循环
- 通信：主线程单向投递（`PostMessage` / `SendNotifyMessage`），不共享可变状态

现有的 `IOverlayBackend` / `IBorderBackend` 已经是平级独立的接口，加第 N 个功能就是
加一个同级接口，不动其他模块——线程化是这个结构的自然延伸，不需要重新设计。

参考：tacky-borders 就是每个边框一个线程（实测 32 线程），我们目前 4 线程。

## 平台

### 10. 覆盖层尺寸不随 DPI 缩放

`LayoutEngine` 输出的全是物理像素，Direct2D 渲染目标钉死在 96 DPI（1 DIP = 1 物理像素）。
所以 `drawer.thickness=34` 在任何缩放下都是 34 个物理像素：在 100% 屏上正常，在 200% 屏上
只有观感上的一半大。设置对话框是缩放的（`Metrics::Scale`），覆盖层和边框不是。

拷到别人机器上这一条最明显：高 DPI 笔记本上书签条和边框会显得偏细偏小，用户得自己把
`drawer.thickness`、`border.width` 往上调。

要修就是让 `LayoutEngine` 输出逻辑像素、由后端按每个窗口所在显示器的 DPI 换算，同时保留
「渲染目标钉 96 DPI」这一点（那是为了避免 rc2 那次整体缩放错位）。改动面不小，涉及所有
尺寸类设置的语义，所以先记着。

### 11. macOS 后端

仍是脚手架。Core 已经与平台无关，需要实现 `IWindowBackend` / `IOverlayBackend` /
`IBorderBackend` 的 Cocoa 版本。
