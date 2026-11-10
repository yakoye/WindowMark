# 保留旧命令入口，统一使用单 EXE 发布流程。
& (Join-Path (Split-Path -Parent $PSScriptRoot) 'make-package.ps1') -Release
exit $LASTEXITCODE
