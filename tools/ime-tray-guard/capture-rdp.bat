@echo off
call "%~dp0capture.bat" -Label console-rdp
exit /b %errorlevel%
