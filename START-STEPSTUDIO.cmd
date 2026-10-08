@echo off
rem ============================================================
rem  Launch STEPStudio 4.7.0 with the correct working directory.
rem  Working dir MUST be the folder containing the exe
rem  (see bin/PACKAGE_README.txt: "启动快捷方式的起始位置应为 STEPStudio.exe 所在目录").
rem ============================================================
cd /d "%~dp0STEPStudio 4.7.0\bin"
if not exist "STEPStudio 4.7.0.exe" (
  echo [ERROR] STEPStudio 4.7.0.exe not found in:
  echo         %CD%
  pause
  exit /b 1
)
echo Starting STEPStudio 4.7.0 ...
start "" "STEPStudio 4.7.0.exe"
