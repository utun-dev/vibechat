@echo off
setlocal EnableExtensions
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_windows.ps1" %*
exit /b %ERRORLEVEL%
