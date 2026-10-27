@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-container-lab.ps1"
echo.
echo Isolated lab only. Native taskbar attachment is disabled. Read README.md.
pause
