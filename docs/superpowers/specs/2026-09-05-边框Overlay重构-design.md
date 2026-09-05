# 边框 Overlay 重构设计

## 一句话

**Z-Order 只读，不写。** 把「边框窗口该插在 z 序哪一格」这个和整个系统抢全局链表的时序
问题，换成「这条边框哪几段该画出来」这个纯几何问题。

## 为什么要换

现在每个被跟踪的窗口配一个边框 HWND，靠 `SetWindowPos` 把它精确插进 Windows 的全局
z 序链表里。这件事天然会和窗口激活、TopMost、弹窗、系统窗口、以及其他进程的
`SetWindowPos` 互相竞争——`SetWindowPos` 改的是那条**全局**链，不是某个目标窗口的私有
附件层。

实测下来的具体后果（都有日志或 trace 佐证）：

- **`SetWindowPos` 会静默失败**：返回 TRUE、`GetLastError()` 为 0、z 序和 topmost 位
  纹丝不动。换锚点、加 `SWP_FRAMECHANGED`、从别的进程调用，全都推不动；而同一进程同一
  线程的其他边框窗口做同样的调用都正常，两者的 ex/style/owner/parent/线程/cloaked/
  showCmd 属性逐项相同。**没有任何办法从返回值察觉这次失败**，只能读回状态验证。
- **十几个边框互相踩**：每次焦点切换，每个边框都要重排，而每一次 `SetWindowPos` 都改变
  全局链表，后一个的插入打乱前一个刚排好的位置。窗口越多越乱。
- **UIPI 挡住高完整性窗口**：任务管理器实测 25 秒内拒绝 141 次，且**不区分方向**——往它
  下面插同样不行。
- **判据有两个来源**：事件驱动的 `model_.active` 和实时的 `GetForegroundWindow()`，切换
  途中会分家，同一个窗口在相邻两次调用里走不同分支，来回拉扯。

这些不是某一次调用写错，是模型本身的问题。

## 新模型

```
Windows 原始窗口
      │  只读取
      ▼
┌──────────────────┐
│ Desktop Snapshot │  foreground / z-order / bounds / visible / cloaked
└────────┬─────────┘
         ▼
┌──────────────────┐
│ Visibility Engine│  非激活：边框区域 - 上方所有窗口的并集
│                  │  激活  ：完整边框，不裁剪，最后画
└────────┬─────────┘
         ▼
┌──────────────────┐
│ Monitor Overlay  │  每显示器一个透明 topmost 画布，只画线
└──────────────────┘
```

全系统只保留**每个显示器一个** overlay 窗口（不是整个虚拟桌面一个超大窗口——那样跨屏
DPI 和坐标都会更麻烦）。

### 遮挡算法

核心就一行：

```
VisibleBorder(target) = BorderRegion(target) - Union(所有排在 target 上面的窗口)
```

从上往下扫一遍即可，不需要两两比较：

```
occlusion = 空
按 z 序从上到下遍历每个窗口 w:
    Border(w) = 边框环(w) - occlusion      // 激活窗口跳过这一步，画完整边框
    occlusion += w 的矩形
```

边框只有 2–4px，是四条细带，**不需要真的做 Region 运算**，四条 RECT 做区间裁剪就够。
桌面上 10–30 个窗口，这个计算对 CPU 不构成负担。

### 激活窗口

```
if (hwnd == snapshot.foreground) 画完整边框，不做 occlusion，最后画
```

正好落到最初的要求上：「哪个激活，就把哪个四边画出来，画到最顶端」——而非激活边框
一并保留下来了。

### 单一真相：Snapshot

WinEvent 钩子**只负责通知「桌面状态脏了」**，不再携带状态：

```
EVENT_SYSTEM_FOREGROUND / LOCATIONCHANGE / SHOW / HIDE / MINIMIZE
        └──> dirty = true
```

真正渲染时现取一份快照，**同一帧只认这一份**：

```cpp
struct WindowSnapshot {
    HWND hwnd;
    RECT frame;        // DWMWA_EXTENDED_FRAME_BOUNDS
    bool visible;
    bool minimized;
    bool cloaked;
    bool maximized;
    int  zIndex;
};
```

这样彻底消灭 `model_.active` 与 `GetForegroundWindow()` 分家的 split-brain。WinEvent 的
语义变成「叫你起来重新看一眼」，而不是「事件里的值就是真相」。

## 实测约束（会影响实现，别踩）

**一、性能是这个方案的主要风险，必须最先验证。**
现在纯移动只做一次 `SetWindowPos`、不重绘，拖动跟随中位数 3.0ms、p99 15.7ms、一帧内
100%。改成 overlay 后每次移动都要**重绘并提交位图**，全屏 2048×1152×4 ≈ 9.4MB/帧。
必须用 `UpdateLayeredWindowIndirect` 的 `prcDirty` 只提交变化的四条细带。**这条不验证
就动工，可能推倒重来。**

**二、Overlay 自己仍要待在 topmost 层末尾**，好让右键菜单、输入法候选框、任务栏、悬浮
小窗压在它上面。关键是它**创建时就带 `WS_EX_TOPMOST`**，此后只需层内换位——今天验证过
这个模式可靠，而会卡死的恰恰是「从普通层提进 topmost 层」，overlay 永远不做这个动作。

**三、bounds 用 `DWMWA_EXTENDED_FRAME_BOUNDS`，不用 `GetWindowRect`。**
后者包含不可见的 resize border（本机 125% 下实测 8px），还有 DPI virtualization。
但要注意：**窗口动画期间两者不同步**——`GetWindowRect` 立刻返回终值而 DWM 边界还在路上，
那一刻算出的内缩量是错的但非零，缓存下来会一直错到下次重标定（v0.4.8 修过一轮）。

**四、必须查 `DWMWA_CLOAKED`。** 切到别的虚拟桌面时，其他桌面的窗口 `IsWindowVisible`
仍返回 true，只看它会让 z 序和遮挡计算全乱。

**五、最大化窗口不画边框**（v0.4.9 已生效）。它贴着工作区边缘，外面没有画边框的那 3px，
夹回屏幕内之后边框和窗口边界完全重合，四条边一个像素都露不出来。

**六、锁屏要暂停。** 用 `WTSRegisterSessionNotification` 收 `WM_WTSSESSION_CHANGE`：
`WTS_SESSION_LOCK` 挂起 overlay，`WTS_SESSION_UNLOCK` 重建快照再恢复。否则 `LockApp.exe`
会以前台窗口的身份混进快照。

## 明确排除的方案

**跨进程 owned window**（把边框设成目标窗口的 owned window，靠 Windows 保证 owned 永远
在 owner 上面）。技术上可行且 z 序天然正确，但 owner 会是 chrome.exe / explorer.exe /
Code.exe，而边框属于 WindowMark.exe——跨进程的 owner/owned 关系会隐式把两边线程的输入
队列绑在一起，而且这种关系可以传递。为了画几条线，把自己和一堆应用的 UI 线程耦合起来，
风险远大于收益。

## 方案对比

| 方案 | 激活边框 | 非激活边框 | z 序冲突 | 复杂度 | 结论 |
|---|---|---|---|---|---|
| 每窗口一个 HWND（现状） | 好 | 不稳定 | 严重 | 高 | 放弃 |
| 只画激活窗口 | 最好 | 无 | 无 | 最低 | 留作兼容模式 |
| **单 Overlay + Occlusion** | **最好** | **很好** | **基本无** | 中 | **主方案** |
| 跨进程 owned window | 好 | 好 | 少 | 中 | 排除（线程耦合） |

## 分阶段

**第一阶段（最小可用）**：矩形窗口遮挡 + 每显示器一个 Overlay + 激活窗口完整边框。
先不处理半透明窗口、异形窗口、圆角窗口的精确遮挡。这一版就应该能一次性消掉「夹层、
乱跳、互相踩、切换闪烁」。

**做之前先做性能验证**（见约束一）：写一个最小 overlay，测拖动时的重绘延迟，确认脏矩形
提交能把开销压到和现在同量级。达不到就先调整渲染策略，不要直接铺开。

**第二阶段**：圆角窗口的遮挡精度、异形窗口、多 DPI 混合下的边界。

## 会被删掉的东西

主路径上这些全部移除：

- 每窗口一个 border HWND
- 激活窗口专用的 topmost border HWND（今天加的）
- `SyncZOrder` / `ResyncIfDrifted` / `ForceTopmost` / `LastTopmostWindow` /
  `LowestOwnedDialog` / `IsAdjacentTo`
- z 序守望轮询（500ms 定时器）与它的 message-only 窗口
- 卡死检测与边框重建（`IsStuck` / `RecreateStuckBorders`）

留下的只有：

```
Move / Resize / Foreground / Z-order 变化
        ↓  重新取快照
     重新计算可见段
        ↓
       重画
```
