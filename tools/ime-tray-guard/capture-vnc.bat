@echo off
call "%~dp0capture.bat" -Label console-vnc
exit /b %errorlevel%
