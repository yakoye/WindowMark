#pragma once

// ClipKeeper 的身份，三方共用：ClipKeeper 自己（注册窗口类、收消息）、WindowMark
// （查状态、发消息）、卸载器（卸载前停掉它）。字符串在三处各写一份必然漂移，
// AppIdentity.h 当初就是为这个而存在的。
//
// 单独一个文件而不是塞进 AppIdentity.h：那个文件是 WindowMark 自己的身份，混进另一个
// 产品的字符串会让两者的生命周期纠缠在一起。ClipKeeper 是独立进程、独立版本号。

namespace windowmark::clipkeeper {

inline constexpr wchar_t kExeName[] = L"ClipKeeper.exe";

// 主窗口类名。WindowMark 靠它回答两个问题：进程在不在跑（FindWindowW 是否非空）、
// 面板是不是可见（IsWindowVisible）。托盘菜单那一项的对勾就是后者。
inline constexpr wchar_t kWindowClass[] = L"ClipKeeperMainWindow";

// 单例。原项目没有任何保护，起两个实例会有两份都插进 Clipboard Viewer Chain：
// 链结构乱掉，两份缓存互相抢救援。
inline constexpr wchar_t kSingletonMutex[] = L"Local\\ClipKeeper.Singleton.v1";

// 两个都要 RegisterWindowMessageW 之后再用。
//
// ShowPanel：把收起到托盘的面板叫回来。托盘菜单用它，第二个实例启动时也用它。
//   （收起面板不需要新消息——WM_CLOSE 的既有行为就是隐藏窗口留在托盘里继续守护。）
// RequestQuit：真正退出。只有卸载器用，因为运行中的 exe 删不掉。托盘菜单不用它：
//   停止守护是在 ClipKeeper 自己的面板里做的事。
inline constexpr wchar_t kShowPanelMessage[] = L"ClipKeeper.ShowPanel.v1";
inline constexpr wchar_t kRequestQuitMessage[] = L"ClipKeeper.RequestQuit.v1";

} // namespace windowmark::clipkeeper
