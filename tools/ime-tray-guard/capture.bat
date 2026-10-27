@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0capture.ps1" %*
set "result=%errorlevel%"
echo.
echo Capture exit code: %result%
pause
exit /b %result%
