@echo off
pushd "%~dp0\..\.."


echo ETCP Monitor Build Script for MSYS2 UCRT64
echo ===========================================
echo.

REM Default MSYS2 path
set MSYS2_PATH=C:\msys64

REM Use environment variable if set
if not "%MSYS2_ROOT%"=="" set MSYS2_PATH=%MSYS2_ROOT%
if not "%MSYS2%"=="" set MSYS2_PATH=%MSYS2%

REM Check if msys2_shell.cmd exists
if not exist "%MSYS2_PATH%\msys2_shell.cmd" (
    echo [ERROR] MSYS2 not found at %MSYS2_PATH%
    echo Please install MSYS2 from https://www.msys2.org/
    echo or set MSYS2_ROOT environment variable.
    pause
    exit /b 1
)

echo [INFO] MSYS2 found at: %MSYS2_PATH%
echo [INFO] Starting build process...
echo [INFO] Log file: tools\etcpmon\build.log
echo.

REM Get current directory (for use in MSYS2)
set "CURRENT_DIR=%CD%"

REM Launch MSYS2 UCRT64 shell and run build script with real-time logging
REM Using tee to show output in console AND save to log
call "%MSYS2_PATH%\msys2_shell.cmd" -ucrt64 -here -defterm -no-start -c "cd \"%CURRENT_DIR%/tools/etcpmon\" && bash build.sh 2>&1"

set BUILD_RESULT=%ERRORLEVEL%

echo.
echo ===========================================
if %BUILD_RESULT% == 0 (
    echo [SUCCESS] Build completed successfully!
    echo [INFO] Executable: tools\etcpmon\etcpmon.exe
    
    REM Check if exe exists and show size
    if exist "tools\etcpmon\etcpmon.exe" (
        for %%F in ("tools\etcpmon\etcpmon.exe") do (
            echo [INFO] Size: %%~zF bytes
        )
    )
) else (
    echo [FAILED] Build failed with error code: %BUILD_RESULT%
    echo [INFO] Check tools\etcpmon\build.log for full details
)
echo ===========================================
echo.

pause
