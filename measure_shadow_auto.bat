@echo off
REM Measure how much bigger a window's rect is than the part you can actually see.
REM
REM Why this matters: some windows paint their drop shadow INSIDE their own window rect
REM (Qt popups, Windows 11 flyouts, GTK windows). The rect is 14-40 px bigger than the
REM visible panel on every side, and nothing in Win32 or DWM reports by how much. Two
REM things go wrong because of it:
REM   - this window's own outline sits detached from its visible edge
REM   - other windows' outlines break open a gap WIDER than this window looks
REM
REM How to use:
REM   1. Put the window on screen. A popup panel is fine - keep the mouse on it.
REM   2. Double-click this file.
REM   3. Hold the mouse still over that window for the 5 second countdown.
REM   4. Copy the printed line into settings.conf under tracking.shadow_insets.
REM      Separate multiple entries with a VERTICAL BAR, not a comma - commas are
REM      already used inside one entry (left,top,right,bottom).
REM   5. Restart WindowMark.
REM
REM ASCII only on purpose: cmd.exe reads .bat in the system ANSI code page, so UTF-8
REM Chinese in here comes out as mojibake and breaks parsing.

setlocal
cd /d "%~dp0"
chcp 65001 >nul

py tools\auto-shadow-inset.py %*

echo.
echo Press any key to close...
pause >nul
endlocal
