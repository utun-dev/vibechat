@echo off
setlocal EnableExtensions

rem ── Проверка прав администратора ──
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Требуются права администратора. Запрос UAC...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b %errorlevel%
)

rem ── Каталог скрипта = каталог vibechat.exe ──
cd /d "%~dp0"

set "EXE=%~dp0vibechat.exe"
if not exist "%EXE%" (
    echo [ОШИБКА] Не найден: %EXE%
    echo Запустите скрипт из папки с vibechat.exe
    pause
    exit /b 1
)

rem ── Идемпотентно: удалить старое правило, добавить новое ──
netsh advfirewall firewall delete rule name="vibechat" >nul 2>&1
netsh advfirewall firewall add rule name="vibechat" dir=in action=allow program="%EXE%" enable=yes profile=any

if %errorlevel% equ 0 (
    echo [OK] Правило "vibechat" добавлено: входящие подключения разрешены.
    echo   %EXE%
) else (
    echo [ОШИБКА] Не удалось добавить правило (код %errorlevel%).
)
pause
