@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 (
    echo [ERROR] Cannot open the script directory.
    pause
    exit /b 1
)

set "MSYS2_PATH=C:\msys64"
if not "%MSYS2_ROOT%"=="" set "MSYS2_PATH=%MSYS2_ROOT%"
if not "%MSYS2%"=="" set "MSYS2_PATH=%MSYS2%"
set "PROBE_LOG=%CD%\cc-probe.log"
set "PROBE_RESULT=1"

> "%PROBE_LOG%" echo [INFO] MSYS2: %MSYS2_PATH%
if not exist "%MSYS2_PATH%\msys2_shell.cmd" (
    >> "%PROBE_LOG%" echo [ERROR] msys2_shell.cmd not found. Set MSYS2_ROOT to the MSYS2 directory.
    goto finish
)

echo [INFO] Checking the UCRT64 C compiler and Opus build...
call "%MSYS2_PATH%\msys2_shell.cmd" -ucrt64 -here -defterm -no-start -c "bash ./check_cc.sh" >> "%PROBE_LOG%" 2>&1
set "PROBE_RESULT=%ERRORLEVEL%"

:finish
>> "%PROBE_LOG%" echo [INFO] MSYS2 shell exit code: %PROBE_RESULT%
type "%PROBE_LOG%"
echo.
echo [INFO] Log: "%PROBE_LOG%"
if not "%PROBE_RESULT%"=="0" echo [ERROR] Compiler or Opus check failed. See the log above.
if /i not "%~1"=="--no-pause" pause
popd
exit /b %PROBE_RESULT%
