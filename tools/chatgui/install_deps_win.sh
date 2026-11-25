#!/usr/bin/env bash
# Подготовка зависимостей GUI в MSYS2 UCRT64: проверяет пакеты и устанавливает отсутствующие через pacman.
# Если все пакеты уже есть, завершает работу без обращения к сети.
set -euo pipefail
GUI_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -f "$GUI_DIR/../../proxy.sh" ]; then
    . "$GUI_DIR/../../proxy.sh" || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi

[[ ${MSYSTEM:-} = UCRT64 ]] || { echo 'ERROR: GUI dependencies require MSYS2 UCRT64' >&2; exit 1; }
command -v pacman >/dev/null || { echo 'ERROR: pacman not found; run the build from MSYS2 UCRT64' >&2; exit 1; }

CHECK_ONLY=0
if [[ $# = 1 && $1 = --check ]]; then
    CHECK_ONLY=1
elif [[ $# != 0 ]]; then
    echo 'ERROR: usage: install_deps_win.sh [--check]' >&2
    exit 1
fi

echo 'INFO: Проверка установленных инструментов и библиотек MSYS2 UCRT64...'
required=(make git curl patch tar xz autoconf-wrapper automake-wrapper)
if python_info=$(python3 -c 'import sys; print(sys.executable, sys.version.split()[0]); sys.exit(sys.version_info < (3, 9))' 2>/dev/null); then
    echo "INFO: Используется доступный Python 3.9+: $python_info"
else
    required+=(python)
fi
for name in gcc cmake ninja pkgconf openssl zlib ffmpeg qt6-base qt6-tools qt6-translations qt6-imageformats qt6-multimedia syntax-highlighting; do
    required+=("mingw-w64-ucrt-x86_64-$name")
done
installed=$(pacman -Qq) || { echo 'ERROR: cannot query installed MSYS2 packages' >&2; exit 1; }
declare -A present=()
while IFS= read -r name; do [[ -z "$name" ]] || present["$name"]=1; done <<< "$installed"
missing=()
for name in "${required[@]}"; do [[ -n ${present[$name]:-} ]] || missing+=("$name"); done
if (( ${#missing[@]} == 0 )); then
    echo "INFO: Все необходимые пакеты (${#required[@]}) уже установлены; установка пропущена."
    exit 0
fi

echo "INFO: Проверено пакетов: ${#required[@]}; уже установлено: $((${#required[@]} - ${#missing[@]})); нужно установить: ${#missing[@]}."
for name in "${missing[@]}"; do
    case "${name#mingw-w64-ucrt-x86_64-}" in
        make) purpose='выполнение сборочных правил Makefile' ;;
        git) purpose='получение исходников и определение версии проекта' ;;
        curl) purpose='скачивание исходных зависимостей по HTTPS' ;;
        patch) purpose='применение исправлений к сторонним библиотекам' ;;
        tar|xz) purpose='распаковка архивов исходников' ;;
        python) purpose='подготовка зависимостей и запуск проверок сборки' ;;
        autoconf-wrapper|automake-wrapper) purpose='генерация configure и Makefile' ;;
        gcc) purpose='компиляторы C/C++ и стандартные библиотеки' ;;
        cmake|ninja) purpose='конфигурация и сборка GUI, rlottie и Qt Multimedia' ;;
        pkgconf) purpose='поиск библиотек и их флагов компиляции' ;;
        openssl) purpose='криптография и защищённые соединения' ;;
        zlib) purpose='распаковка анимированных эмодзи TGS' ;;
        ffmpeg) purpose='обработка, кодирование и декодирование аудио/видео' ;;
        qt6-base) purpose='основные библиотеки графического интерфейса Qt' ;;
        qt6-tools) purpose='инструменты разработки и сборки ресурсов Qt' ;;
        qt6-translations) purpose='переводы стандартных элементов Qt' ;;
        qt6-imageformats) purpose='поддержка изображений, включая WebP' ;;
        qt6-multimedia) purpose='камера, захват экрана и воспроизведение медиа' ;;
        syntax-highlighting) purpose='подсветка исходного кода в сообщениях' ;;
        *) purpose='дополнительный пакет сборочного окружения' ;;
    esac
    printf 'INFO:   %s — %s\n' "$name" "$purpose"
done
if [[ "$CHECK_ONLY" = 1 ]]; then
    echo 'INFO: Локальная проверка завершена; недостающие пакеты не устанавливались (код 3).'
    exit 3
fi
echo 'INFO: Запуск pacman; ниже показаны скачивание и установка пакетов с их зависимостями.'
pacman -S --needed --noconfirm "${missing[@]}" || {
    echo 'ERROR: dependency installation failed; check the pacman output. If repositories are outdated, run pacman -Syu and retry.' >&2
    exit 1
}
echo 'INFO: Установка завершена; проверка наличия всех необходимых пакетов...'
pacman -Q "${required[@]}" >/dev/null || { echo 'ERROR: GUI dependencies still missing after installation' >&2; exit 1; }
echo "INFO: Все ${#required[@]} необходимых пакетов установлены и проверены."
