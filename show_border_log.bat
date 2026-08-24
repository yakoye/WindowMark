@echo off
REM Live watch: prints a line the moment WindowMark draws a border on a window.
REM
REM How to use:
REM   1. Double-click this file. Leave the window open.
REM   2. Go to the app and make the popup appear (hover and hold still).
REM   3. The line shows up here immediately.
REM   4. Press Ctrl+C when done.
REM
REM Recording is switched on automatically by creating diag.on below.

setlocal
set D=%LOCALAPPDATA%\WindowMark

if not exist "%D%" mkdir "%D%"
if not exist "%D%\diag.on" type nul > "%D%\diag.on"

echo ============================================================
echo   WindowMark - live border watch
echo ============================================================
echo.
echo   Recording is ON.  Log: %D%\diag.log
echo.
echo   Now go make the popup appear, then look here.
echo   Ctrl+C to stop.
echo.
echo   Nothing below this line yet means: nothing has been
echo   bordered since you opened this window.
echo ------------------------------------------------------------
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command "$p = Join-Path $env:LOCALAPPDATA 'WindowMark\diag.log'; while (-not (Test-Path $p)) { Start-Sleep -Milliseconds 500 }; Get-Content -LiteralPath $p -Wait -Tail 0 -Encoding UTF8 | Where-Object { $_ -match 'style=|hwnd=' } | ForEach-Object { Write-Host $_ }"

endlocal
