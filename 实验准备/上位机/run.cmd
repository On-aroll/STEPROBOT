@echo off
setlocal
pushd "%~dp0" || exit /b 1
if not exist "bin\host\StepRobot.App.exe" (
    echo Please run build.ps1 first.
    popd
    exit /b 2
)
"bin\host\StepRobot.App.exe" %*
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
