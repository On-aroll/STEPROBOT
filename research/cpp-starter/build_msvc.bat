@echo off
setlocal
rem Compile+link against the vendor static lib. A successful link proves the toolchain and ABI.
rem ASCII-only on purpose: cmd.exe reads .bat with the system ANSI codepage (GBK here).
call "D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "SDK_DIR=%~dp0..\..\_extract\sdk\SR.SDK_C++_X64_Win_V2.4.1.20250801"
pushd "%~dp0" || exit /b 1
if not exist build mkdir build
pushd build || (popd & exit /b 1)
if not exist obj mkdir obj
cl /nologo /W3 /EHsc /std:c++17 /MD /I "%SDK_DIR%" "..\src\main.cpp" /Fe:robot_probe.exe /Foobj\ /link "%SDK_DIR%\Robot-SDK.lib" ws2_32.lib
if errorlevel 1 (echo == BUILD FAILED == & popd & popd & exit /b 1)
echo == BUILD OK ==
cl 2>&1 | findstr /i Version
popd
popd
exit /b 0
