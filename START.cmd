@echo off
setlocal
pushd "%~dp0"
where pwsh.exe >nul 2>nul
if errorlevel 1 (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Run9.ps1" -Mode Smoke
) else (
  pwsh.exe -NoProfile -File "%~dp0scripts\Run9.ps1" -Mode Smoke
)
set "PMUX_EXIT=%ERRORLEVEL%"
echo.
echo Exit code: %PMUX_EXIT%
popd
if /I not "%~1"=="--no-pause" pause
exit /b %PMUX_EXIT%
