# ClipKeeper 集成 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ClipKeeper 作为独立 exe 并入 WindowMark 仓库与安装包，由托盘菜单开关它的面板。

**Architecture:** ClipKeeper 保持独立进程（理由见 spec：Viewer Chain 依赖启动顺序，且它每次剪贴板变化都要 `SendMessageTimeout` 阻塞最多 250ms）。WindowMark 通过窗口类名查状态、通过注册消息控制它，两边共用一个标识头文件。安装器负责一起装、卸载器负责先停再删。

**Tech Stack:** C++20 / Win32 / CMake / MSVC（`/W4 /WX`，警告即错误）

## Global Constraints

- 设计依据：`docs/superpowers/specs/2026-09-03-ClipKeeper集成-design.md`
- ClipKeeper 版本号独立，保持 `0.1.0`，不跟 WindowMark 的 `0.4.x`
- ClipKeeper **不开机自启**，也不随 WindowMark 启动；它自己的「开机启动」勾选框保留
- 已实测：ClipKeeper 源码在 C++20 + `/W4 /WX` 下零代码警告，不为它放宽编译口径
- 托盘菜单项「剪贴板守护」开关的是**面板**，不是进程；对勾 = 面板可见
- 编译零警告，`python tools/check-escapes.py` 必须通过
- 含反斜杠的内容一律用 Write 工具或 Write 出来的 python 脚本改，不走 shell heredoc/sed；
  **也不要用 `python -c` 传中文**（本轮已踩过，shell 传递会让匹配失败）
- 构建：`cmake --build build --config Release --parallel`
- 装机验证：`powershell -NoProfile -ExecutionPolicy Bypass -File .\reinstall.ps1`
- 源码来源：`C:\Users\color\Downloads\ClipKeeper\ClipKeeper-dev\src\`（原目录不动）

---

### Task 1: 源码进仓库，构建产出两个 exe

**Files:**
- Create: `src/clipkeeper/main.cpp`、`clipboard.cpp`、`clipboard.h`、`resource.h`、`ClipKeeper.rc`（从原项目复制）
- Create: `src/clipkeeper/`（连同 `.rc` 引用的图标资源，见 Step 2）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: 构建目标 `ClipKeeper`，产出 `build/Release/ClipKeeper.exe`

- [ ] **Step 1: 复制源码**

用 Write 工具或 python 脚本复制（不要用 shell cp 传中文路径）：

```
C:\Users\color\Downloads\ClipKeeper\ClipKeeper-dev\src\main.cpp       -> src/clipkeeper/main.cpp
C:\Users\color\Downloads\ClipKeeper\ClipKeeper-dev\src\clipboard.cpp  -> src/clipkeeper/clipboard.cpp
C:\Users\color\Downloads\ClipKeeper\ClipKeeper-dev\src\clipboard.h    -> src/clipkeeper/clipboard.h
C:\Users\color\Downloads\ClipKeeper\ClipKeeper-dev\src\resource.h     -> src/clipkeeper/resource.h
C:\Users\color\Downloads\ClipKeeper\ClipKeeper-dev\src\ClipKeeper.rc  -> src/clipkeeper/ClipKeeper.rc
```

- [ ] **Step 2: 确认 .rc 引用的资源都在**

```bash
grep -n "ICON\|\.ico\|resource.h" src/clipkeeper/ClipKeeper.rc
```

`.rc` 里引用的 `.ico` 若不在 `src/clipkeeper/` 下，一并复制过来并把 `.rc` 里的相对路径改对。
`IDI_APPICON` 定义在 `resource.h`，`main.cpp:wWinMain` 用它加载窗口图标。

- [ ] **Step 3: 加构建目标**

`CMakeLists.txt` 中，在 `add_executable(WindowMarkInspect ...)` 那一段之后插入：

```cmake
    # 剪贴板守护。独立进程而不是 WindowMark 的一部分：它抢占 Clipboard Viewer Chain 的
    # 链头，而链头是「谁最后注册谁在前面」——WindowMark 开机自启，必然早于 ToDesk，
    # 排在后面就来不及在剪贴板被清空前存下内容。它每次剪贴板变化还要 SendMessageTimeout
    # 往下游传（最多 250ms），放进 WindowMark 的主消息循环会拖慢边框刷新。
    # 版本号独立于 WindowMark，用它自己的 .rc。
    add_executable(ClipKeeper WIN32
        src/clipkeeper/main.cpp
        src/clipkeeper/clipboard.cpp
        src/clipkeeper/ClipKeeper.rc
    )
    target_include_directories(ClipKeeper PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/clipkeeper
        ${CMAKE_CURRENT_SOURCE_DIR}/src/shared
    )
    target_compile_definitions(ClipKeeper PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(ClipKeeper PRIVATE user32 gdi32 shell32 advapi32 comctl32)
    if(MSVC)
        target_compile_options(ClipKeeper PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    endif()
```

- [ ] **Step 4: 构建，确认零警告**

```bash
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

预期：`build/Release/ClipKeeper.exe` 产出，零错误零警告。（探针已验证过这份源码在
`/W4 /WX` 下干净，若这里报警告，多半是 `.rc` 或 include 路径没配好。）

- [ ] **Step 5: 跑一次确认它能独立工作**

```bash
./build/Release/ClipKeeper.exe
```

预期：面板出现，托盘出现 ClipKeeper 图标。复制一段文字，面板的历史列表里应出现一条。
手动关掉它再继续。

- [ ] **Step 6: 提交**

```bash
git add src/clipkeeper CMakeLists.txt
git commit -m "ClipKeeper 源码进仓库，构建产出独立 exe"
```

---

### Task 2: 共享标识头 + 单例 + 两个注册消息

**Files:**
- Create: `src/shared/ClipKeeperIdentity.h`
- Modify: `src/clipkeeper/main.cpp`

**Interfaces:**
- Produces:
  - `windowmark::clipkeeper::kWindowClass` = `L"ClipKeeperMainWindow"`
  - `windowmark::clipkeeper::kExeName` = `L"ClipKeeper.exe"`
  - `windowmark::clipkeeper::kSingletonMutex` = `L"Local\\ClipKeeper.Singleton.v1"`
  - `windowmark::clipkeeper::kShowPanelMessage` = `L"ClipKeeper.ShowPanel.v1"`
  - `windowmark::clipkeeper::kRequestQuitMessage` = `L"ClipKeeper.RequestQuit.v1"`

- [ ] **Step 1: 写共享标识头**

`src/shared/ClipKeeperIdentity.h`：

```cpp
#pragma once

// ClipKeeper 的身份，三方共用：ClipKeeper 自己（注册窗口类、收消息）、WindowMark
// （查状态、发消息）、卸载器（卸载前停掉它）。字符串在三处各写一份必然漂移，
// AppIdentity.h 当初就是为这个而存在的。
//
// 单独一个文件而不是塞进 AppIdentity.h：那个文件是 WindowMark 自己的身份，混进另一个
// 产品的字符串会让两者的生命周期纠缠在一起。

namespace windowmark::clipkeeper {

inline constexpr wchar_t kExeName[] = L"ClipKeeper.exe";

// 主窗口类名。WindowMark 靠它查「有没有在跑」和「面板是不是可见」。
inline constexpr wchar_t kWindowClass[] = L"ClipKeeperMainWindow";

// 单例。原项目没有任何保护，起两个实例会有两份都插进 Clipboard Viewer Chain。
inline constexpr wchar_t kSingletonMutex[] = L"Local\\ClipKeeper.Singleton.v1";

// 都要 RegisterWindowMessageW 之后再用。
// ShowPanel：把收起到托盘的面板叫回来。菜单和第二个实例都用它。
// RequestQuit：真正退出。只有卸载器用——运行中的 exe 删不掉。
inline constexpr wchar_t kShowPanelMessage[] = L"ClipKeeper.ShowPanel.v1";
inline constexpr wchar_t kRequestQuitMessage[] = L"ClipKeeper.RequestQuit.v1";

} // namespace windowmark::clipkeeper
```

- [ ] **Step 2: 让 ClipKeeper 用共享的窗口类名**

`src/clipkeeper/main.cpp` 顶部加 include，并把原来的 `kClassName` 换成共享常量：

```cpp
#include "ClipKeeperIdentity.h"

namespace ck = windowmark::clipkeeper;
```

原第 14 行 `constexpr wchar_t kClassName[] = L"ClipKeeperMainWindow";` 删掉，把文件里所有
`kClassName` 改为 `ck::kWindowClass`（`wWinMain` 里注册窗口类和 `CreateWindowExW` 两处）。

- [ ] **Step 3: 加单例保护**

`wWinMain` 开头，在 `RegisterClassExW` 之前插入：

```cpp
    // 起两个实例会有两份都插进 Clipboard Viewer Chain，链结构乱掉，两份缓存互相抢救援。
    HANDLE singleton = ::CreateMutexW(nullptr, TRUE, ck::kSingletonMutex);
    if (singleton && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已经有一个在跑：把它的面板叫出来，自己退场。
        // 不用 SetForegroundWindow 抢前台——本机实测不可靠（前台锁），让已有实例
        // 自己 ShowWindow 就够了。
        if (const UINT showPanel = ::RegisterWindowMessageW(ck::kShowPanelMessage)) {
            if (HWND existing = ::FindWindowW(ck::kWindowClass, nullptr)) {
                ::PostMessageW(existing, showPanel, 0, 0);
            }
        }
        ::CloseHandle(singleton);
        return 0;
    }
```

`return` 前（函数末尾 `return static_cast<int>(msg.wParam);` 之前）释放：

```cpp
    if (singleton) ::CloseHandle(singleton);
```

- [ ] **Step 4: 处理两个注册消息**

注册消息的值不是编译期常量，进不了 `switch` 的 `case`。在 `WndProc` 的 `switch (msg)`
**之前**插入：

```cpp
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 注册消息的值要运行时才知道，所以在 switch 之前拦。函数级 static 保证只注册一次。
    static const UINT kShowPanel = ::RegisterWindowMessageW(ck::kShowPanelMessage);
    static const UINT kRequestQuit = ::RegisterWindowMessageW(ck::kRequestQuitMessage);

    // RegisterWindowMessageW 失败返回 0，而 0 是 WM_NULL——不挡掉的话每个 WM_NULL
    // 都会被当成这两个命令。
    if (kShowPanel != 0 && msg == kShowPanel) {
        ShowMainWindow();
        return 0;
    }
    if (kRequestQuit != 0 && msg == kRequestQuit) {
        // g_exiting 是既有标志：置位后 WM_CLOSE 不再收起到托盘，而是走到
        // DefWindowProc -> WM_DESTROY，那里会 ChangeClipboardChain 干净地退出监听链。
        g_exiting = true;
        ::DestroyWindow(hwnd);
        return 0;
    }

    switch (msg) {
```

- [ ] **Step 5: 构建**

```bash
cmake --build build --config Release --parallel
python tools/check-escapes.py
```

预期：零警告，转义检查通过。

- [ ] **Step 6: 验证单例与两个消息**

把下面的脚本写到 scratchpad 再运行（**用 Write 工具写文件，不要 `python -c`**）：

```python
# 验证 ClipKeeper 的单例、ShowPanel、RequestQuit。全程 PostMessage，不碰键鼠。
import ctypes, os, subprocess, time
from ctypes import wintypes

user32 = ctypes.WinDLL('user32', use_last_error=True)
EXE = os.path.abspath('build/Release/ClipKeeper.exe')
CLS = 'ClipKeeperMainWindow'

def find():
    return user32.FindWindowW(ctypes.c_wchar_p(CLS), None)

def count_procs():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ClipKeeper.exe'],
                         capture_output=True, text=True).stdout
    return out.count('ClipKeeper.exe')

subprocess.Popen([EXE]); time.sleep(2.5)
h = find()
print('  启动后找到窗口        ', 'OK' if h else '不符')
print('  面板可见              ', 'OK' if user32.IsWindowVisible(h) else '不符')

# 再起一个：进程数应当还是 1
subprocess.Popen([EXE]); time.sleep(2.0)
n = count_procs()
print('  第二个实例被挡掉      ', 'OK' if n == 1 else '不符 (%d 个)' % n)

# 收起面板（WM_CLOSE 的既有行为），再用 ShowPanel 叫回来
user32.PostMessageW(h, 0x0010, 0, 0); time.sleep(1.0)
print('  WM_CLOSE 收起面板     ', 'OK' if not user32.IsWindowVisible(h) else '不符')
print('  进程仍在              ', 'OK' if count_procs() == 1 else '不符')

show = user32.RegisterWindowMessageW(ctypes.c_wchar_p('ClipKeeper.ShowPanel.v1'))
user32.PostMessageW(h, show, 0, 0); time.sleep(1.0)
print('  ShowPanel 叫回面板    ', 'OK' if user32.IsWindowVisible(h) else '不符')

quit_msg = user32.RegisterWindowMessageW(ctypes.c_wchar_p('ClipKeeper.RequestQuit.v1'))
user32.PostMessageW(h, quit_msg, 0, 0); time.sleep(1.5)
print('  RequestQuit 真正退出  ', 'OK' if count_procs() == 0 else '不符')
```

六项全 OK 才算过。

- [ ] **Step 7: 提交**

```bash
git add src/shared/ClipKeeperIdentity.h src/clipkeeper/main.cpp
git commit -m "ClipKeeper 补单例互斥量与 ShowPanel/RequestQuit 两个注册消息"
```

---

### Task 3: 托盘菜单「剪贴板守护」

**Files:**
- Modify: `src/platform/windows/WinControlWindow.h`（命令 id、handler）
- Modify: `src/platform/windows/WinControlWindow.cpp`（菜单项、派发、自检）
- Modify: `src/platform/windows/WinMain.cpp`（handler 实现）

**Interfaces:**
- Consumes: Task 2 的 `windowmark::clipkeeper::kWindowClass` / `kExeName` / `kShowPanelMessage`
- Produces: `WinControlWindow::Handlers::onClipKeeper`（`std::function<void()>`）

- [ ] **Step 1: 命令 id 与 handler 声明**

`WinControlWindow.h`，在 `kConfigPathCommand = 1016;` 之后加：

```cpp
    static constexpr UINT kClipKeeperCommand = 1017;
```

`Handlers` 结构里，在 `onConfigPath` 之前加：

```cpp
        // 剪贴板守护。开关的是它的面板而不是进程——进程的启停在它自己的面板里做。
        std::function<void()> onClipKeeper;
```

- [ ] **Step 2: 菜单项**

`WinControlWindow.cpp` 顶部加 include：

```cpp
#include "ClipKeeperIdentity.h"
```

在挂载「窗口置顶」子菜单那一行之后（`AppendMenuW(menu, MF_POPUP | (pinningEnabled_ ? ...),
reinterpret_cast<UINT_PTR>(pinning), L"窗口置顶");` 的闭合大括号之后）、`MF_SEPARATOR`
之前插入：

```cpp
    // 剪贴板守护是独立进程，这一项开关的是它的面板。对勾 = 面板当前可见，与点击行为
    // 一一对应。状态每次开菜单实时查，不缓存：用户可能从 ClipKeeper 自己的托盘收起
    // 或退出它。
    //
    // 它是顶层唯一能真正点击切换的功能项——另外几个带子菜单，MF_POPUP 项没有命令 ID，
    // 点击即展开子菜单，没法兼作开关。
    {
        namespace ck = windowmark::clipkeeper;
        const HWND panel = FindWindowW(ck::kWindowClass, nullptr);
        const bool visible = panel != nullptr && IsWindowVisible(panel);
        AppendMenuW(menu, MF_STRING | (visible ? MF_CHECKED : MF_UNCHECKED),
                    kClipKeeperCommand, L"剪贴板守护");
    }
```

- [ ] **Step 3: 命令派发与启动自检**

派发处，在 `case kConfigPathCommand:` 之前加：

```cpp
        case kClipKeeperCommand:     handler = &handlers_.onClipKeeper; break;
```

启动自检的 `wired` 数组里，在 `onConfigPath` 那一行之前加：

```cpp
        {L"onClipKeeper", static_cast<bool>(handlers_.onClipKeeper)},
```

- [ ] **Step 4: handler 实现**

`WinMain.cpp` 顶部加 include：

```cpp
#include "ClipKeeperIdentity.h"
```

在 `handlers.onConfigPath = ...` 之前插入：

```cpp
    handlers.onClipKeeper = [&]() {
        namespace ck = windowmark::clipkeeper;
        const HWND panel = FindWindowW(ck::kWindowClass, nullptr);

        if (!panel) {
            // 没在跑：启动它，它自带面板。exe 与 WindowMark.exe 同目录。
            const auto exe = windowmark::win::InstalledExePath().parent_path() / ck::kExeName;
            std::error_code ec;
            if (!std::filesystem::exists(exe, ec)) {
                MessageBoxW(control.NativeHandle(),
                            L"找不到 ClipKeeper.exe。\n\n"
                            L"它应当与 WindowMark.exe 在同一个目录，"
                            L"重新运行一次安装程序即可补上。",
                            L"WindowMark", MB_OK | MB_ICONWARNING);
                return;
            }
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            std::wstring command = exe.wstring();
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            } else {
                MessageBoxW(control.NativeHandle(), L"启动 ClipKeeper 失败。", L"WindowMark",
                            MB_OK | MB_ICONWARNING);
            }
            return;
        }

        if (IsWindowVisible(panel)) {
            // 收起面板。WM_CLOSE 是它的既有行为：隐藏窗口，留在托盘里继续守护。
            PostMessageW(panel, WM_CLOSE, 0, 0);
            return;
        }

        // 在托盘里：叫出来。
        if (const UINT showPanel = RegisterWindowMessageW(ck::kShowPanelMessage)) {
            PostMessageW(panel, showPanel, 0, 0);
        }
    };
```

- [ ] **Step 5: 构建并装机**

```bash
cmake --build build --config Release --parallel
python tools/check-escapes.py
powershell -NoProfile -ExecutionPolicy Bypass -File .\reinstall.ps1
```

装机后**手动把 `build/Release/ClipKeeper.exe` 拷到安装目录**（Task 4 之前安装器还不管它）：

```bash
cp build/Release/ClipKeeper.exe "$LOCALAPPDATA/Programs/WindowMark/"
```

- [ ] **Step 6: 验证三种点击状态**

把脚本写到 scratchpad 再跑（用 Write 工具写文件）。菜单命令可以直接投给隐藏的控制窗口，
不必真的去点托盘：

```python
# 验证「剪贴板守护」菜单项的三种状态。全程 PostMessage，不碰键鼠。
import ctypes, subprocess, time
user32 = ctypes.WinDLL('user32', use_last_error=True)
CFG = 1017  # kClipKeeperCommand

def ctrl():   return user32.FindWindowW(ctypes.c_wchar_p('WindowMark.Control'), None)
def panel():  return user32.FindWindowW(ctypes.c_wchar_p('ClipKeeperMainWindow'), None)
def procs():
    out = subprocess.run(['tasklist','/FI','IMAGENAME eq ClipKeeper.exe'],
                         capture_output=True, text=True).stdout
    return out.count('ClipKeeper.exe')

def click():
    user32.PostMessageW(ctrl(), 0x0111, CFG, 0)   # WM_COMMAND
    time.sleep(2.5)

print('  起点：未运行          ', 'OK' if procs() == 0 else '不符')
click()
print('  点一下：进程起来+面板  ', 'OK' if procs() == 1 and user32.IsWindowVisible(panel()) else '不符')
click()
print('  再点：面板收起,进程还在', 'OK' if procs() == 1 and not user32.IsWindowVisible(panel()) else '不符')
click()
print('  再点：面板回来        ', 'OK' if user32.IsWindowVisible(panel()) else '不符')

q = user32.RegisterWindowMessageW(ctypes.c_wchar_p('ClipKeeper.RequestQuit.v1'))
user32.PostMessageW(panel(), q, 0, 0); time.sleep(1.5)
click()
print('  退出后再点能重新启动  ', 'OK' if procs() == 1 else '不符')
user32.PostMessageW(panel(), q, 0, 0)   # 收尾
```

另外**右击一次托盘菜单**人眼确认两件事：

1. 「剪贴板守护」在「窗口置顶」下面、分隔线上面，面板开着时有对勾
2. **点「暂停所有」之后，ClipKeeper 不受影响**——进程还在、面板状态不变。spec 明确要求
   它不被总开关管：截图恰恰是最需要剪贴板保护的时刻，跟着一起停是帮倒忙。这一条靠
   「不写代码」实现，正因为如此更要正面确认一次，不能假定没写就一定没影响。

- [ ] **Step 7: 提交**

```bash
git add src/platform/windows/WinControlWindow.h src/platform/windows/WinControlWindow.cpp src/platform/windows/WinMain.cpp
git commit -m "托盘加「剪贴板守护」，开关 ClipKeeper 的面板"
```

---

### Task 4: 安装器一起装，卸载器先停再删

**Files:**
- Modify: `src/installer/InstallerCommon.h` / `.cpp`（把进程查找与停止参数化）
- Modify: `src/installer/SetupMain.cpp`（复制 ClipKeeper.exe）
- Modify: `src/installer/UninstallMain.cpp`（停 ClipKeeper、删文件、清它的 Run 键）

**Interfaces:**
- Consumes: Task 2 的 `windowmark::clipkeeper::*`
- Produces:
  - `setup::FindRunningInstances(const wchar_t* exeName)` — 原无参版改为带参
  - `setup::StopRunningInstances(unsigned graceMs, const wchar_t* exeName, const wchar_t* windowClass, const wchar_t* quitMessage)` — 同上，带默认值

- [ ] **Step 1: 参数化进程查找与停止**

`InstallerCommon.h` 里把两个声明改成（默认值让现有调用点一字不用改）：

```cpp
std::vector<RunningInstance> FindRunningInstances(
    const wchar_t* exeName = app::kMainExeName);

// 先礼后兵：给目标窗口投退出消息，超时未退再 TerminateProcess。
// 带参数是为了同一套逻辑也能停 ClipKeeper——它有自己的 exe 名、窗口类和退出消息。
bool StopRunningInstances(unsigned graceMs,
                          const wchar_t* exeName = app::kMainExeName,
                          const wchar_t* windowClass = app::kControlWindowClass,
                          const wchar_t* quitMessage = app::kRequestQuitMessage);
```

`InstallerCommon.h` 顶部确认已 include `AppIdentity.h`（默认参数要用到那些常量）。

- [ ] **Step 2: 改实现**

`InstallerCommon.cpp:46` 那行改为用参数：

```cpp
            if (_wcsicmp(entry.szExeFile, exeName) != 0) continue;
```

函数签名同步改成 `FindRunningInstances(const wchar_t* exeName)`。

`PostQuitToControlWindow` 现在只能传一个 `LPARAM`，而参数化后需要「窗口类 + 消息」两个值。
改成传结构体指针：

```cpp
namespace {

struct QuitTarget {
    const wchar_t* windowClass;
    UINT message;
};

BOOL CALLBACK PostQuitToControlWindow(HWND hwnd, LPARAM lParam) {
    const auto* target = reinterpret_cast<const QuitTarget*>(lParam);
    wchar_t className[64]{};
    if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) == 0) return TRUE;
    if (_wcsicmp(className, target->windowClass) != 0) return TRUE;
    PostMessageW(hwnd, target->message, 0, 0);
    return TRUE;
}

} // namespace
```

`StopRunningInstances` 的签名与前两处调用改为：

```cpp
bool StopRunningInstances(unsigned graceMs, const wchar_t* exeName,
                          const wchar_t* windowClass, const wchar_t* quitMessage) {
    auto instances = FindRunningInstances(exeName);
    if (instances.empty()) return true;

    if (const UINT message = RegisterWindowMessageW(quitMessage)) {
        QuitTarget target{windowClass, message};
        EnumWindows(PostQuitToControlWindow, reinterpret_cast<LPARAM>(&target));
    }
```

函数其余部分不变。

- [ ] **Step 3: 安装器复制 ClipKeeper.exe**

`SetupMain.cpp` 顶部加 include：

```cpp
#include "ClipKeeperIdentity.h"
```

在 `const auto sourceUninstaller = setup::LocatePayload(app::kUninstallExeName);` 之后加：

```cpp
    const auto sourceClipKeeper = setup::LocatePayload(windowmark::clipkeeper::kExeName);
```

在 `const auto targetUninstaller = installDir / app::kUninstallExeName;` 之后加：

```cpp
    const auto targetClipKeeper = installDir / windowmark::clipkeeper::kExeName;
```

在卸载器那段 `CopyFileTo` 之后，照同样的写法加（**缺它不算致命**，主程序仍可用，
所以走 warning 而不是 error）：

```cpp
    // 剪贴板守护是可选组件：装不上只是少一个功能，主程序照常工作，所以只警告。
    if (sourceClipKeeper.empty()) {
        warnings += L"未找到 " + std::wstring(windowmark::clipkeeper::kExeName) +
                    L"，剪贴板守护将不可用。\n";
    } else if (!setup::CopyFileTo(sourceClipKeeper, targetClipKeeper, error)) {
        warnings += L"复制 " + std::wstring(windowmark::clipkeeper::kExeName) +
                    L" 失败：" + error + L"\n";
    }
```

`SetupMain.cpp:213` 现有的 `setup::StopRunningInstances(4000);` 之后加一行，装之前也要停掉
运行中的 ClipKeeper（否则覆盖不了它的 exe）：

```cpp
    setup::StopRunningInstances(4000, windowmark::clipkeeper::kExeName,
                                windowmark::clipkeeper::kWindowClass,
                                windowmark::clipkeeper::kRequestQuitMessage);
```

- [ ] **Step 4: 卸载器停它、删它、清它的 Run 键**

`UninstallMain.cpp` 顶部加 include：

```cpp
#include "ClipKeeperIdentity.h"
```

`UninstallMain.cpp:170` 的 `setup::StopRunningInstances(4000);` 之后加：

```cpp
    // 运行中的 exe 删不掉，所以先停。
    setup::StopRunningInstances(4000, windowmark::clipkeeper::kExeName,
                                windowmark::clipkeeper::kWindowClass,
                                windowmark::clipkeeper::kRequestQuitMessage);
```

**不需要单独加删除 ClipKeeper.exe 的代码**：卸载走的是 `setup::RemoveTree(installDir)`
（`InstallerCommon.cpp:314`），整个目录 `remove_all`，装在里面的东西都会跟着走。**唯一要做的
就是上面那步「先停掉它」**——运行中的 exe 会让 `remove_all` 失败，那才是残留的来源。

要单独加的只有一处：清掉它自己的自启动键值。用户可能开过它面板上的「开机启动」勾选框，
不清就会留下一个指向已删除 exe 的启动项：

```cpp
    // ClipKeeper 有自己的「开机启动」开关，写的是 Run\ClipKeeper。卸载 WindowMark 时
    // 一并清掉，否则会留下一个指向已删除 exe 的启动项。
    if (HKEY runKey = nullptr;
        RegOpenKeyExW(HKEY_CURRENT_USER, app::kRunKeyPath, 0, KEY_SET_VALUE, &runKey) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(runKey, L"ClipKeeper");
        RegCloseKey(runKey);
    }
```

- [ ] **Step 5: 打包脚本带上它**

确认 `reinstall.ps1` 把 `ClipKeeper.exe` 放到安装器能找到的位置（`LocatePayload` 查的是
安装器 exe 同目录）。若 `reinstall.ps1` 是直接调 `build/Release/WindowMarkSetup.exe`，
那三个 exe 本来就在同一个目录，无需改动——运行一次确认即可。

- [ ] **Step 6: 构建 + 装机 + 验证**

```bash
cmake --build build --config Release --parallel
python tools/check-escapes.py
powershell -NoProfile -ExecutionPolicy Bypass -File .\reinstall.ps1
ls "$LOCALAPPDATA/Programs/WindowMark/"
```

预期：安装目录里有 `WindowMark.exe`、`WindowMarkUninstall.exe`、`ClipKeeper.exe`。

再验证卸载（**这一步会真的卸载，验证完要重新装回来**）：

| 步骤 | 期望 |
|---|---|
| 启动 ClipKeeper，然后运行卸载器 | 卸载器先把它停掉，安装目录清空无残留 |
| 卸载后查 `HKCU\...\Run\ClipKeeper` | 已删除 |
| 重新 `reinstall.ps1` | 三个 exe 都回来 |

- [ ] **Step 7: 提交**

```bash
git add src/installer CMakeLists.txt
git commit -m "安装器一起装 ClipKeeper，卸载器先停再删并清它的自启动键"
```

---

### Task 5: 文档、版本、发布

**Files:**
- Modify: `src/shared/AppIdentity.h`、`CMakeLists.txt`（版本 → `0.4.6`）
- Modify: `README.md`、`CHANGELOG.md`、`VALIDATION.md`

- [ ] **Step 1: 升版本**

`AppIdentity.h` 的 `kProductVersion` 与 `CMakeLists.txt` 的 `project(...)` 同步改成 `0.4.6`。
两处不一致会在 configure 阶段直接失败，这是既有的防线。
**ClipKeeper 自己的版本不动**，保持 `0.1.0`。

- [ ] **Step 2: README 加一节**

在「窗口置顶」一节之后加「剪贴板守护」一节，必须写清楚这四件事：

- 它解决什么：ToDesk 等远程会话中截图后 `Ctrl+V` 失效
- **必须在连上 ToDesk 之后再开启**，否则排在 ToDesk 后面，来不及在剪贴板被清空前存下内容
- 它是**独立进程**，有自己的托盘图标和面板；托盘菜单那一项开关的是**面板**，不是进程，
  停止守护在它自己的面板里做
- 它面板上的「开机启动」勾选框**不建议开**：开了就回到「先于 ToDesk 启动」的状态

同时更新托盘菜单示意图，把「剪贴板守护」加进去。

- [ ] **Step 3: CHANGELOG 加 v0.4.6 段**

两件事：托盘顶层显示各功能启用状态（含 header 去除与实测宽度数据），以及 ClipKeeper 集成
（含「为什么是独立进程」的两条理由：Viewer Chain 顺序、250ms 阻塞）。

- [ ] **Step 4: VALIDATION 记录本轮实测**

菜单宽度那组数据、单例与两个消息的六项验证、菜单项三种状态的验证、安装卸载验证。

- [ ] **Step 5: 全量构建 + 单测 + 装机**

```bash
cmake --build build --config Release --parallel
./build/Release/windowmark_core_tests.exe
./build/Release/windowmark_autostart_tests.exe
python tools/check-escapes.py
powershell -NoProfile -ExecutionPolicy Bypass -File .\reinstall.ps1
```

- [ ] **Step 6: 打包、提交、发布**

按 v0.4.5 的既有流程：`dist/WindowMark-v0.4.6-win64/` 装齐**五个 exe**（比上一版多
`ClipKeeper.exe`）加 README/CHANGELOG 加 tools，压包，校验哈希，提交打 tag 推送，
`gh release create`，最后下载回来比对 SHA256。

`publish_release.bat` 里的 `TAG` / `ZIP` / `NOTES` 三处改成 `v0.4.6`。

---

## 风险与回滚

| 风险 | 影响 | 应对 |
|---|---|---|
| ClipKeeper 排在 ToDesk 之后 | 救援失效，且不报错 | 文档写明必须连上 ToDesk 后再开启；面板关掉再开一次即可重回链头 |
| 卸载时 ClipKeeper 没停 | exe 删不掉，安装目录留残留 | 卸载器先发 RequestQuit，超时 TerminateProcess（沿用既有的先礼后兵逻辑） |
| `.rc` 资源没跟过来 | 构建失败或图标丢失 | Task 1 Step 2 专门检查 |
| 参数化 `StopRunningInstances` 改坏了 WindowMark 自己的卸载 | 卸载器停不掉主程序 | 默认参数保持原值，现有调用点一字不改；Task 4 Step 6 验证完整卸载流程 |

单个任务出问题：`git revert` 该任务的提交即可。Task 1–4 之间没有数据迁移，回滚是干净的。
