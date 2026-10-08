@echo off
setlocal
pushd "%~dp0" || exit /b 1
echo Recovery demo: software test stub only. No Studio or robot connection.
echo Native SDK and controller validation are still pending.
if not exist "bin\demo-only\StepRobot.App.exe" (
    echo Missing demo executable. Check the recovery report.
    popd
    pause
    exit /b 2
)
"bin\demo-only\StepRobot.App.exe" --demo --seconds 3
set "RESULT=%ERRORLEVEL%"
echo Exit code: %RESULT%
popd
pause
exit /b %RESULT%
