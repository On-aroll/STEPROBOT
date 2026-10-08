@echo off
setlocal
rem ---- Build the SDK control program WITHOUT vcvars64.bat (reg.exe is sandbox-blocked).
rem ---- INCLUDE/LIB are assembled by hand:
rem   MSVC 14.44 headers/libs : D:\VSBuildTools\VC\Tools\MSVC\14.44.35207
rem   UCRT headers            : D:\Tool\Windhawk\Compiler\include          (mingw-w64 ucrt)
rem   UCRT import lib         : D:\Tool\Windhawk\Compiler\x86_64-w64-mingw32\lib\libucrtbase.a
rem   Win32 import libs       : libkernel32.a / libws2_32.a (mingw-w64, plain COFF import libs)
set "MSVC=D:\VSBuildTools\VC\Tools\MSVC\14.44.35207"
set "WH=D:\Tool\Windhawk\Compiler"
set "SDKH=D:\Desktop\STEPROBOT\_setup\winsdk\Include"
set "SDK_DIR=%~dp0..\..\_extract\sdk\SR.SDK_C++_X64_Win_V2.4.1.20250801"
set "INCLUDE=%MSVC%\include;%SDKH%\ucrt;%SDKH%\um;%SDKH%\shared"
set "LIB=%MSVC%\lib\x64;%SDKLIB%;D:\Desktop\STEPROBOT\_setup\winsdk\lib-x64"
set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"

pushd "%~dp0" || exit /b 1
if not exist build mkdir build
pushd build || (popd & exit /b 1)
if not exist obj2 mkdir obj2

cl /nologo /W3 /EHsc /std:c++17 /MD ^
  /I "%SDK_DIR%" ^
  "..\src\main.cpp" ^
  /Fe:robot_full.exe /Foobj2\ ^
  /link ^
  "%SDK_DIR%\Robot-SDK.lib" msvcrt.lib vcruntime.lib msvcprt.lib
if errorlevel 1 (echo == BUILD FAILED == & popd & popd & exit /b 1)
echo == BUILD OK ==
popd
popd
exit /b 0
