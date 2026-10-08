@echo off
setlocal
call "D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "SDK_DIR=%~dp0..\..\..\_extract\sdk\SR.SDK_C++_X64_Win_V2.4.1.20250801"
pushd "%~dp0" || exit /b 1
if not exist bin mkdir bin
cl /nologo /W3 /EHsc /std:c++17 /MD /LD /I "%SDK_DIR%" bridge.cpp /Fobin\bridge.obj /Febin\StepRobot.Native.dll /link "%SDK_DIR%\Robot-SDK.lib" ws2_32.lib
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
