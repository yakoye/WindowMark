@echo off
setlocal
pushd "%~dp0"
echo Read-only taskbar capture. No injection, no settings changes, no screenshots.
echo Switch between a local editor and your existing RDP/VNC/MobaXterm window.
echo Capture lasts about two minutes. Keep both IME transitions visible.
if not exist results mkdir results
set "RUN_ID=%RANDOM%-%RANDOM%"
WindowMarkImeTrayProbe.exe --out "results\anchors-%RUN_ID%.jsonl" --samples 120 --interval-ms 1000 --delay-ms 5000 --include-app-geometry
echo Capture exit code: %ERRORLEVEL%
echo Return the results folder and your Windows version, DPI and monitor count.
popd
pause
