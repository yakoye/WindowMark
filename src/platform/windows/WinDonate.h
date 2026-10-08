#pragma once

// 「请我喝杯咖啡」。
//
// 入口放在关于框的脚注里，一行小字，不占正文——用户的原话是「不要太显眼，免得太违和」。
// 点了才弹出这个窗口，里面是收款码。
//
// 图片**编进 exe**（RCDATA），不是外挂文件：发出去的那份是单独一个 exe 的绿色版，带个
// 外挂图片就不成立了。放 res\donate-wechat.png / res\donate-alipay.png，CMake 配置时看
// 哪个在就编哪个；两个都不在，关于框里那行脚注也不出现，整套功能等于不存在。

#include <windows.h>

namespace windowmark::win {

// 二维码在不在（编进来了没有）。关于框据此决定要不要显示那行脚注。
[[nodiscard]] bool HasDonateCodes();

// 弹出收款码窗口。模态，owner 可以是 nullptr。
void ShowDonateWindow(HWND owner);

// 关于框脚注左边那颗红心。调用方负责 DestroyIcon。
//
// 为什么要自己画：脚注那行是 SysLink，它只认「非链接=系统文字色、链接=系统链接色」两档，
// 没法把一个字单独染红。把心放进图标那一格，文字整条做成链接，就同时得到红心和蓝字。
[[nodiscard]] HICON CreateHeartIcon(int size);

} // namespace windowmark::win
