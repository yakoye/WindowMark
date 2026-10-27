@echo off
setlocal
pushd "%~dp0"
echo Read-only taskbar capture. No injection, no settings change.
echo Keep the local editor active first, then switch to your existing RDP window.
echo Switch back and forth several times during the next 120 seconds.
if not exist results mkdir results
set "RUN_ID=%RANDOM%-%RANDOM%"
WindowMarkImeTrayProbe.exe --out "results\win10-%RUN_ID%.jsonl" --samples 240 --interval-ms 500 --delay-ms 5000
echo Capture exit code: %ERRORLEVEL%
echo Please return the results folder. No screenshots are taken.
popd
pause
