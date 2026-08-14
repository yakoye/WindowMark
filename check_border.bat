@echo off
rem Double-click when a border edge looks missing. It waits 5 seconds so you can bring the
rem window you care about to the front, then reports whether the border was drawn at all,
rem whether it sits above its target, and for every sampled pixel that is the wrong colour,
rem which window is covering it.
rem
rem   check_border.bat                 foreground window, 5s countdown
rem   check_border.bat -Title Setup    a window whose title contains "Setup", no countdown
rem   check_border.bat -Delay 10       longer countdown
rem
rem ASCII only: cmd.exe reads .bat in the system ANSI code page, so UTF-8 Chinese in here
rem breaks parsing. The Chinese output comes from check-border.ps1.

setlocal
cd /d "%~dp0"
chcp 65001 >nul

if "%~1"=="" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check-border.ps1" -Delay 5
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check-border.ps1" %*
)

echo.
echo Press any key to close...
pause >nul
