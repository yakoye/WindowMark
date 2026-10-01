@echo off
REM Live watch on WindowMark's diagnostic log.
REM
REM How to use:
REM   1. Double-click this file. Leave the window open.
REM   2. Reproduce the problem (hover a bookmark, move a window, whatever is wrong).
REM   3. The lines show up here immediately.
REM   4. Press Ctrl+C when done.
REM
REM Recording is switched on automatically (a diag.on file next to settings.conf) and
REM stays on until you turn it off:
REM   powershell -ExecutionPolicy Bypass -File tools\watch-diag-log.ps1 -Off
REM
REM The log lives next to the running WindowMark.exe for a portable copy, or in
REM %LOCALAPPDATA%\WindowMark for an installed one. The PowerShell script finds it; this
REM file stays ASCII-only because cmd.exe reads .bat in the system ANSI code page.

setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\watch-diag-log.ps1" %*
endlocal
