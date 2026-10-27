@echo off
call "%~dp0capture.bat" -Label console-mobaxterm
exit /b %errorlevel%
