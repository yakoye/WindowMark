@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-tsf-trial.ps1"
echo.
echo Read README.md and keep the results folder for analysis.
pause
