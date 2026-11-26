Библиотека для построения mesh-сети и mesh чат с поддержкой:
- групп
- рации, голосоывх, видеозвонков, трансляций
- kanban доски

web: https://utun.freedev.app

1. Скачать [репозиторий VibeChat на GitHub](https://github.com/utun-dev/vibechat):

   ```bash
   git clone https://github.com/utun-dev/vibechat.git
   cd vibechat
   ```

2. Собрать ядро и VibeChat для Linux:

   ```bash
   ./build_linux_full.sh -j4
   ```

   Скрипт подготовит зависимости, применит патчи и выполнит сборку.
   Недостающие инструменты установит через apt/sudo либо сообщит, что нужно исправить.
   Для сборки только ядра добавьте `--no-gui`.

   Повторная сборка только ядра:

   ```bash
   ./build.sh --no-gui -j4
   ```

   Тесты при обычной и полной сборке не компилируются. На Linux x86_64 оба скрипта автоматически
   используют OpenSSL из `/opt/openssl-3.5`, если он установлен штатным `tools/install_openssl_ubuntu.sh`.
   Для другой установки можно задать переменную окружения `OPENSSL_ROOT_DIR`.

   Для Android на Linux x86_64:

   ```bash
   bash tools/chatgui-android/build.sh noinstall
   ```

   Android-скрипт сам подготовит SDK/NDK и native-зависимости перед сборкой APK.
   При запросе sdkmanager лицензии принимаются в терминале.

   Для Windows 10 (1809+) / Windows 11 x64, из PowerShell или обычного cmd:

   ```powershell
   .\build_full.bat
   ```

   Для первой подготовки достаточно распакованных исходников и доступа в интернет. Скрипт сам скачает MSYS2,
   обновит её в два прохода при первичной установке пакетов и установит GCC, Autotools, Python, CMake/Ninja, OpenSSL,
   Qt, FFmpeg и Inno Setup. Установщики MSYS2 и Inno Setup проверяются по закреплённой SHA-256.
   Сборка создаёт ядро, GUI и `tools/chatgui/build/installer/VibeChat-*-win64-setup.exe`.

   Повторная сборка: `build.bat`; только GUI: `tools\chatgui\build.bat`.
   Число потоков: `build.bat -Jobs 8`; только установка инструментов: `build.bat -PrepareOnly`.
   Если все зависимости уже подготовлены, сборка использует локальные файлы без проверки обновлений и доступа к сети.
   Это относится и к `build_full.bat`: полная пересборка не требует обновления пакетов.
   Обновить инструменты явно: `build.bat -UpdateTools` (или добавить `-PrepareOnly`, чтобы не запускать сборку).
   В уже установленной MSYS2 обычная сборка при недостающих зависимостях перечислит их и остановится без сетевых операций.
   Установить недостающее явно: `build.bat -PrepareOnly`. Первый запуск на чистой Windows по-прежнему готовит окружение автоматически.
   Подходит доступный `python3` версии 3.9+, включая UCRT64; отдельный MSYS-пакет `python` в этом случае не требуется.
   MSYS2 по умолчанию устанавливается в `C:\msys64`; другой абсолютный путь без пробелов
   и кириллицы задаётся через `MSYS2_ROOT` (старый `MSYS2` также учитывается).
   Загрузки и локальный Inno Setup хранятся в `%LOCALAPPDATA%\uTun\build-tools`.
   Уже установленный Inno Setup 6.3+ используется повторно; можно указать путь к `ISCC.exe` через `ISCC`.
   Логи: `build_windows_setup.log`, `build_win.log` / `build_full.log` / `build_chatgui.log`.

3. Проверить Linux-сборку:

   ```bash
   ./check.sh
   ```

   Скрипт сам соберёт ядро и тестовые программы перед запуском проверок; первая компиляция не входит в таймаут тестов.
   На Windows запускайте `check.bat`: он собирает и запускает тесты в установленном MSYS2 UCRT64.

Готовые программы: `src/utun` и `tools/chatgui/build/vibechat`.
Подготовка Android-зависимостей и параметры скрипта: [doc/dependencies.txt](doc/dependencies.txt).
