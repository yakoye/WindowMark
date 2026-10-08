#pragma once

// Resource ids shared by the .rc and the code that loads from it.
//
// 1 on purpose: Explorer shows the icon with the *lowest* id as a program's file icon, so
// the application icon must keep this number even if others are added later.
#define IDI_APPICON 1

// 收款码（RCDATA，PNG 原样编进来）。CMake 看 res\donate-wechat.png / res\donate-alipay.png
// 在不在，决定编不编；两个都没有的话这两个 id 在 exe 里就不存在，关于框里那行脚注也不出现。
#define IDR_DONATE_BOTH   100   // 一张合图（两个码并排）；有它就只用它
#define IDR_DONATE_WECHAT 101
#define IDR_DONATE_ALIPAY 102
