@echo off
setlocal
pushd "%~dp0" || exit /b 1
if not exist "bin\host\StepRobot.App.exe" (
    echo Please run build.ps1 first.
    popd
    pause
    exit /b 2
)
"bin\host\StepRobot.App.exe" --demo --seconds 3
set "RESULT=%ERRORLEVEL%"
echo Exit code: %RESULT%
popd
pause
exit /b %RESULT%
