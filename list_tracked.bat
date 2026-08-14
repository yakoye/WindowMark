@echo off
rem Double-click and it watches for 30 seconds, then lists every window that actually got
rem a border during that time - transient flyouts first, with their class names ready to
rem paste back. Use it when something gets an outline that should not have one.
rem
rem While it runs, do the things you want to catch: type some Chinese so the candidate list
rem pops up, press Win+Space, click the tray overflow chevron, open a context menu.
rem
rem   list_tracked.bat             watch 30s (default)
rem   list_tracked.bat -Watch 60   watch 60s
rem   list_tracked.bat -Watch 0    snapshot this instant only
rem
rem ASCII only: cmd.exe reads .bat in the system ANSI code page, so UTF-8 Chinese in here
rem breaks parsing. The Chinese output comes from list-tracked.ps1.

setlocal
cd /d "%~dp0"
chcp 65001 >nul

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0list-tracked.ps1" %*

echo.
echo Press any key to close...
pause >nul
