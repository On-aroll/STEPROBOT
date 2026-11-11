@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Configure-STEPStudio.ps1" %*
pause
