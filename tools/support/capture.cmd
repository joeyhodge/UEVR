@echo off
setlocal
where pwsh.exe >nul 2>nul
if errorlevel 1 (
    echo Install PowerShell 7 or run capture.ps1 from a 64-bit PowerShell 7 terminal.
    pause
    exit /b 1
)
pwsh.exe -NoLogo -NoProfile -File "%~dp0capture.ps1" %*
set "result=%errorlevel%"
pause
exit /b %result%
