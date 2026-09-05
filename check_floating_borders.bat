@echo off
rem Watch for "floating" borders: a border stuck in the topmost band while its
rem target window is NOT the foreground window. That is exactly the stray blue
rem line that crosses unrelated windows.
rem
rem Double-click this, then switch between windows for 25 seconds
rem (include Task Manager). Any hit is printed immediately.
title WindowMark - floating border check
cd /d "%~dp0"
python "tools\check-floating-borders.py" 25
echo.
pause
