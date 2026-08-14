@echo off
setlocal
cd /d "%~dp0"

echo === WindowMark v0.3.7 build ===
where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cmake not found. Install CMake or add it to PATH.
    exit /b 1
)

if not exist build mkdir build
cmake -S . -B build
if errorlevel 1 exit /b 1

cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1

ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 (
    echo [ERROR] Core tests failed. Not reporting this build as usable.
    exit /b 1
)

echo.
echo Build completed.
echo   App:       %CD%\build\Release\WindowMark.exe
echo   Inspect:   %CD%\build\Release\WindowMarkInspect.exe
echo   Installer: %CD%\build\Release\WindowMarkSetup.exe
echo   Uninstall: %CD%\build\Release\WindowMarkUninstall.exe
echo.
echo Run WindowMarkSetup.exe to install. Installing over a running copy is fine.
endlocal
