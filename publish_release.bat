@echo off
REM Create the GitHub Release for the tag that is already pushed, and upload the zip.
REM
REM Everything else is done already: main is pushed, the v0.4.6 tag is on GitHub, and
REM dist\ holds the package and the release notes. This step needs a GitHub login, which
REM only you can do.
REM
REM If it says you are not logged in, run this once first:
REM     gh auth login
REM and pick GitHub.com -> SSH -> login with a browser.

setlocal
cd /d "%~dp0"

set TAG=v0.4.6
set ZIP=dist\WindowMark-v0.4.6-win64.zip
set NOTES=dist\release-notes-v0.4.6.md

where gh >nul 2>&1
if errorlevel 1 (
  echo gh is not on PATH. Install GitHub CLI first: https://cli.github.com
  goto done
)

if not exist "%ZIP%" (
  echo Missing %ZIP% - run reinstall.ps1 and the packaging step first.
  goto done
)

gh auth status >nul 2>&1
if errorlevel 1 (
  echo.
  echo Not logged in to GitHub. Run this once, then start this file again:
  echo.
  echo     gh auth login
  echo.
  goto done
)

echo Creating release %TAG% ...
gh release create %TAG% "%ZIP%" --title "WindowMark %TAG%" --notes-file "%NOTES%"
if errorlevel 1 (
  echo.
  echo Failed. If the release already exists, upload the asset instead:
  echo     gh release upload %TAG% "%ZIP%" --clobber
) else (
  echo.
  echo Done. Opening it in the browser...
  gh release view %TAG% --web
)

:done
echo.
pause
endlocal
