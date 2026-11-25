Пользовательский выпуск VibeChat для Ubuntu 26.04 x64

Сборка с установленными системными dev-пакетами:
    bash tools/chatgui/build_nix.sh --user-archive -j4

Подготовка зависимостей и полная сборка (sudo используется только на машине сборки):
    bash tools/chatgui/build_nix.sh --full --user-archive -j4

Результат: tools/chatgui/build-user/installer/vibechat-<version>-ubuntu-26.04-amd64.tar.gz
Сборка и публикация архива с обновлением страницы загрузки:
    bash tools/chatgui/publish_user_archive.sh -j4
Скрипт загружает архив в vc@vc:/home/vc/www/app/ и html/index.html в vc@vc:/home/vc/www/index.html.
Сборка отдельная, Release; системные Qt/KF6/FFmpeg/ONNX Runtime. Библиотеки не включаются в архив.
Упаковка отклоняет RPATH приложения, зависимости вне системного runtime и файлы без владельца dpkg.
Минимальные версии ELF-зависимостей определяет dpkg-shlibdeps. Проверяются также системные Qt-плагины,
их пакеты/версии и зависимости; ALSA, PulseAudio, сертификаты и FUSE учитываются отдельно.

Пользователь распаковывает архив и запускает bash install.sh; подробности в README.txt внутри архива.
--check проверяет зависимости без установки. Установка и удаление не требуют sudo, не меняют профиль.
Нужен включённый репозиторий universe. Системные библиотеки при необходимости ставит администратор.
Целевая ОС строго Ubuntu 26.04 amd64; поддержка следующих выпусков требует отдельной проверки ABI.

Проверка установщика:
    python3 tools/chatgui/tests/test_user_installer.py
Нужны Ubuntu 26.04 x64, desktop-file-validate и gio (desktop-file-utils, libglib2.0-bin).
Тест работает во временном HOME, не устанавливает приложение в настоящий профиль пользователя.
Проверка готового архива с первым запуском, SQLite, повторным запуском и сохранением идентичности:
    xvfb-run -a python3 tools/chatgui/tests/test_linux_install_startup.py <путь-к-архиву.tar.gz>
Нужны xvfb и xdotool; запускать обычным пользователем, control-порт 9999 должен быть свободен.
Существующая DEB-упаковка и --installer остаются без изменения.
