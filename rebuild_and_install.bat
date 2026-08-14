@echo off
rem Double-click to build, uninstall the old copy and install the new one.
rem
rem   rebuild_and_install.bat            keep settings.conf
rem   rebuild_and_install.bat -Fresh     delete settings.conf too (to test code defaults)
rem   rebuild_and_install.bat -NoBuild   skip the build, just reinstall
rem
rem ASCII only on purpose: cmd.exe reads .bat in the system ANSI code page, so UTF-8
rem Chinese in here comes out as mojibake and breaks parsing. The Chinese output comes
rem from reinstall.ps1 instead, which is why chcp switches the console to UTF-8 first.

setlocal
cd /d "%~dp0"
chcp 65001 >nul

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0reinstall.ps1" %*
set EXITCODE=%ERRORLEVEL%

echo.
if %EXITCODE% NEQ 0 (
    echo [FAILED] exit code %EXITCODE%
) else (
    echo [DONE]
)
echo.
echo Press any key to close...
pause >nul
exit /b %EXITCODE%
