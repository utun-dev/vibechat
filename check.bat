@echo off
setlocal EnableExtensions

set "CHECK_MSYS_ROOT=C:\msys64"
if defined MSYS2_ROOT set "CHECK_MSYS_ROOT=%MSYS2_ROOT%"
if defined MSYS2 set "CHECK_MSYS_ROOT=%MSYS2%"
if not exist "%CHECK_MSYS_ROOT%\usr\bin\bash.exe" (
    echo ERROR: MSYS2 is not installed in "%CHECK_MSYS_ROOT%". Run build.bat -PrepareOnly first.
    exit /b 1
)

pushd "%~dp0" || exit /b 1
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
set "MSYS2_PATH_TYPE=strict"
echo [INFO] Building and running tests in MSYS2 UCRT64
"%CHECK_MSYS_ROOT%\usr\bin\bash.exe" --login -c "exec bash build.sh --no-gui check"
set "CHECK_RESULT=%ERRORLEVEL%"
popd
exit /b %CHECK_RESULT%
