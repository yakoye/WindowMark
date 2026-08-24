@echo off
REM Measure how much invisible shadow an application paints inside its own window rect.
REM
REM Needed for GTK and other client-side-decorated apps: their outline looks detached
REM because the window rect is bigger than the visible window, and no Windows API says
REM by how much.
REM
REM How to use:
REM   1. Make sure the window is restored, not maximized.
REM   2. Double-click this file.
REM   3. Move the mouse over that window and hold still for the 5 second countdown.
REM   4. Copy the printed line into settings.conf, then restart WindowMark.

setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\measure-shadow-inset.ps1"
echo.
pause
endlocal
