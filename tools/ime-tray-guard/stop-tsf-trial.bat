@echo off
setlocal
cd /d "%~dp0"
"%~dp0WindowMarkTsfSlotTrial.exe" --stop
echo Stop request sent, or no trial is running. Wait for the main launcher to finish.
pause
