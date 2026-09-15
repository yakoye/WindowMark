# WindowMark 运行依赖

## 问题说明

如果在全新安装的 Windows 10 / Windows 11 上运行 `WindowMarkSetup.exe` 或 `WindowMark.exe` 时出现以下错误：

```text
找不到 VCRUNTIME140.dll
找不到 MSVCP140.dll
找不到 VCRUNTIME140_1.dll
```

说明系统中缺少 **Microsoft Visual C++ Redistributable 运行库**。

这不是 WindowMark 文件损坏，安装微软官方 VC++ 运行库即可解决。

---

## Windows 10 / Windows 11 安装什么

WindowMark 当前为 **64 位程序**，Windows 10 和 Windows 11 都安装：

### 必装：VC++ x64 运行库

Microsoft 官方下载：

https://aka.ms/vc14/vc_redist.x64.exe

下载后双击：

```text
vc_redist.x64.exe
```

选择“安装”，安装完成后重新运行 WindowMark 即可。

---

## 可选：x86 运行库

如果电脑上还需要运行其他 32 位 Windows 软件，也可以同时安装：

https://aka.ms/vc14/vc_redist.x86.exe

WindowMark 本身为 64 位程序，通常只安装 x64 版本即可。

---

## ARM64 Windows

如果使用 Windows on ARM，可下载：

https://aka.ms/vc14/vc_redist.arm64.exe

---

## 推荐配置

| 系统 | 建议安装 |
|---|---|
| Windows 10 x64 | `vc_redist.x64.exe` |
| Windows 11 x64 | `vc_redist.x64.exe` |
| Windows 10/11 x64，同时需要运行 32 位软件 | `vc_redist.x64.exe` + `vc_redist.x86.exe` |
| Windows 11 ARM64 | `vc_redist.arm64.exe` |

> 请尽量从 Microsoft 官方链接下载安装，不要从第三方 DLL 网站单独下载 `VCRUNTIME140.dll`、`MSVCP140.dll` 等文件。
