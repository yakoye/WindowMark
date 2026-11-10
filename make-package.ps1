# 单 EXE 绿色版交付；开发文档和测试工具不进入用户附件。
param([switch]$Release, [switch]$Candidate, [switch]$Force)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if ($Release -and $Candidate) { throw 'Release 与 Candidate 不可同时使用' }
$identity = Get-Content src/shared/AppIdentity.h -Raw -Encoding UTF8
if ($identity -notmatch 'kProductVersion\[\]\s*=\s*L"([^"]+)"') { throw '无法读取版本号' }
$version = $Matches[1]
if ($Release -and $version.Contains('-')) { throw '候选版本不能正式发布' }
$source = Join-Path $PSScriptRoot 'build/Release/WindowMark.exe'
if (!(Test-Path $source)) { throw '请先构建 Release' }
if ((Get-Item $source).VersionInfo.ProductVersion -ne $version) { throw '构建版本与源码不一致' }
if ($Release) { $directory = Join-Path $PSScriptRoot "dist/v$version" }
elseif ($Candidate) { $directory = Join-Path $PSScriptRoot "dist/rc/v$version" }
else { $directory = Join-Path $PSScriptRoot ("dist/test/v$version-" + (Get-Date -Format yyyyMMdd-HHmmss)) }
New-Item -ItemType Directory -Force $directory | Out-Null
$target = Join-Path $directory 'WindowMark.exe'
if ((Test-Path $target) -and !$Force) { throw "目标已存在，不覆盖：$target" }
Copy-Item -LiteralPath $source -Destination $target -Force
$hash = (Get-FileHash $source -Algorithm SHA256).Hash
if ((Get-FileHash $target -Algorithm SHA256).Hash -ne $hash) { throw '产物校验失败' }
if ($Release -or $Candidate) {
    foreach ($root in @('C:/Users/color/Downloads/yakoye-release/WindowMark', '//192.168.7.100/Downloads/yakoye-release/WindowMark')) {
        $archive = Join-Path $root "v$version"
        New-Item -ItemType Directory -Force $archive | Out-Null
        $copy = Join-Path $archive 'WindowMark.exe'
        if ((Test-Path $copy) -and (Get-FileHash $copy).Hash -ne $hash) { throw "不同的归档已存在：$copy" }
        Copy-Item -LiteralPath $target -Destination $copy -Force
        if ((Get-FileHash $copy).Hash -ne $hash) { throw "归档校验失败：$copy" }
    }
}
Write-Output "单文件：$target"
Write-Output "SHA256：$hash"
