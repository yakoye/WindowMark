#pragma once

// 装自己、卸自己。
//
// 以前这是两个独立的 exe（WindowMarkSetup.exe / WindowMarkUninstall.exe），于是发布包里
// 并排躺着六个 exe，用户第一眼就不知道该双击哪个。现在整个包只有一个 WindowMark.exe：
// 双击就是运行，`--install` 是安装，`--uninstall` 是卸载，托盘菜单里各有一个入口。
//
// 单文件带来一个好处：安装就是把自己拷过去。不再有「安装程序找不到 WindowMark.exe」这种
// 状态——要安装的那个文件就是正在运行的这个文件。

#include <filesystem>

namespace windowmark::setup {

// 命令行里带 --install / --uninstall 时，在这里把事情做完并返回 true，调用者应当立刻用
// exitCode 退出，不要再去启动应用本体。必须在 COM 初始化之后、抢单实例互斥体之前调用：
// 安装和卸载都要先停掉正在运行的那份，自己不能先把互斥体占上。
bool HandleCommandLine(int& exitCode);

// 当前这份是不是跑在安装目录里。决定托盘菜单显示「安装到系统」还是「卸载」。
[[nodiscard]] bool RunningFromInstallDir();

// 托盘菜单的两个入口。返回 true 表示当前进程应当退出：安装完要把位置让给安装的那份，
// 卸载则已经在临时副本里开工，它马上会来停掉我们。
bool InstallFromTray();
bool UninstallFromTray();

} // namespace windowmark::setup
