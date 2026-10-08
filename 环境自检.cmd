@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Check-Environment.ps1"
set "RESULT=%ERRORLEVEL%"
echo.
echo Check exit code: %RESULT%
pause
exit /b %RESULT%
