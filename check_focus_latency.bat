@echo off
rem Measure how fast the border appears / disappears when you switch windows.
rem
rem Passive: it hooks the foreground event and watches. It never steals focus
rem and never moves any window.
rem
rem Double-click this, then keep clicking back and forth between windows for
rem 30 seconds. The more switches, the better the numbers.
title WindowMark - focus switch latency
cd /d "%~dp0"
python "tools\bench-focus-latency.py" 30
echo.
pause
