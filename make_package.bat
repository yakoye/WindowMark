@echo off
rem Double-click to build the install package (zip) from what is already in build\Release.
rem
rem   make_package.bat            test package into dist\test\ (name carries build time + commit)
rem   make_package.bat -Release   release package dist\WindowMark-v<version>-win64.zip
rem
rem rebuild_and_install.bat already does this after every install; use this one only to
rem package again without reinstalling.
rem
rem ASCII only on purpose: cmd.exe reads .bat in the system ANSI code page, so UTF-8
rem Chinese in here comes out as mojibake and breaks parsing. The Chinese output comes
rem from make-package.ps1 instead, which is why chcp switches the console to UTF-8 first.

setlocal
cd /d "%~dp0"
chcp 65001 >nul

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make-package.ps1" %*
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
