@echo off
setlocal EnableExtensions
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\build_windows.ps1" -GuiOnly %*
exit /b %ERRORLEVEL%
