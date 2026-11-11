@echo off
setlocal
set "STEP_NATIVE=%~dp0native\StepWorkstation.exe"
if not exist "%STEP_NATIVE%" (
    echo STEP Workstation native executable is missing. See README.md for build instructions.
    pause
    exit /b 1
)
start "" "%STEP_NATIVE%" %*
