#!/bin/bash
# Проверяет раскладку Windows runtime настоящим build.sh с подставленными CMake и PE-импортами.
# Во временных каталогах тестирует Qt5/Qt6, DLL/плагины, кеш и ошибки зависимостей; при сбое сохраняет диагностику.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")/.." && pwd)
fixture_root=$(mktemp -d)
trap 'test_rc=$?; if [ "$test_rc" -eq 0 ]; then rm -rf -- "$fixture_root"; else
    echo "ERROR: deployment fixture retained: $fixture_root" >&2
    [ -z "${case_dir:-}" ] || tail -60 "$case_dir/output.log" >&2
fi' EXIT
mkdir -p "$fixture_root/commands" "$fixture_root/lib"
printf 'DLL Name: KERNEL32.dll\n' > "$fixture_root/lib/wintun.dll"
cat > "$fixture_root/commands/cmake" <<'SH'
#!/bin/bash
set -eu
if [ "$1" = --build ]; then
    [ ! -f "$2/vibechat.exe" ] || exit 0
    printf 'DLL Name: Qt%sWidgets.dll\nDLL Name: Qt%sNetwork.dll\n' "$DEPLOY_MAJOR" "$DEPLOY_MAJOR" > "$2/vibechat.exe"
    printf 'DLL Name: libgcc_s_seh-1.dll\nDLL Name: libwinpthread-1.dll\nDLL Name: librlottie.dll\nDLL Name: zlib1.dll\nDLL Name: libcrypto-3.dll\n' >> "$2/vibechat.exe"
else
    while [ "$1" != -B ]; do shift; done
    mkdir -p "$2"
    printf 'Qt%s\n' "$DEPLOY_MAJOR" > "$2/chatgui_qt_package.txt"
    printf '%s\n' "${DEPLOY_CAPTURE:-0}" > "$2/chatgui_capture_enabled.txt"
fi
SH
cat > "$fixture_root/commands/objdump" <<'SH'
#!/bin/bash
printf '%s\n' "$2" >> "$DEPLOY_OBJDUMP_LOG"
if [[ "$(< "$2")" = *BROKEN* ]]; then echo 'invalid PE fixture' >&2; exit 7; fi
printf '%s: file format pei-x86-64\n' "$2"
cat "$2"
SH
chmod +x "$fixture_root/commands/"*

prepare_case() {
    case_dir="$fixture_root/project/Qt$1 package"; prefix="$case_dir/ucrt64"
    mkdir -p "$case_dir/rlottie_build" "$prefix/bin" "$prefix/share/qt$1/plugins/platforms" "$prefix/share/qt$1/plugins/imageformats"
    cp "$fixture_root/commands/objdump" "$prefix/bin/objdump"
    cp "$source_dir/build.sh" "$source_dir/package_win.sh" "$source_dir/compiler_runtime.sh" "$source_dir/runtime_cache.sh" "$case_dir/"
    printf 'DLL Name: KERNEL32.dll\n' > "$case_dir/rlottie_build/librlottie.dll"
    printf 'fixture\n' > "$case_dir/setup_firewall.bat"
    printf 'DLL Name: AUTHZ.dll\n' > "$prefix/bin/Qt$1Core.dll"
    printf 'DLL Name: d3d12.dll\nDLL Name: libpng.dll\n' > "$prefix/bin/Qt$1Gui.dll"
    printf 'DLL Name: WINHTTP.dll\n' > "$prefix/bin/Qt$1Network.dll"
    printf 'DLL Name: Qt%sGui.dll\nDLL Name: Qt%sCore.dll\nDLL Name: libstdc++-6.dll\n' "$1" "$1" > "$prefix/bin/Qt$1Widgets.dll"
    for name in libpng.dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll zlib1.dll libcrypto-3.dll \
        plugin-helper.dll jpeg-helper.dll webp-helper.dll; do
        printf 'DLL Name: KERNEL32.dll\n' > "$prefix/bin/$name"
    done
    printf 'DLL Name: Qt%sGui.dll\nDLL Name: plugin-helper.dll\n' "$1" > "$prefix/share/qt$1/plugins/platforms/qwindows.dll"
    printf 'DLL Name: d3d9.dll\nDLL Name: SHCORE.dll\n' >> "$prefix/share/qt$1/plugins/platforms/qwindows.dll"
    printf 'DLL Name: jpeg-helper.dll\n' > "$prefix/share/qt$1/plugins/imageformats/qjpeg.dll"
    printf 'DLL Name: webp-helper.dll\n' > "$prefix/share/qt$1/plugins/imageformats/qwebp.dll"
    cat > "$prefix/bin/windeployqt$1.exe" <<SH
#!/bin/bash
if [ "\$1" = --version ]; then echo 'windeployqt $1.10.0'; else echo 'Qt$1 deploy' >> "\$DEPLOY_TOOL_LOG"; fi
SH
    chmod +x "$prefix/bin/windeployqt$1.exe"
}
run_case() {
    : > "$case_dir/objdump.log"
    (cd "$case_dir"; PATH="$fixture_root/commands:$PATH" DEPLOY_MAJOR="$1" DEPLOY_CAPTURE="${2:-0}" DEPLOY_TOOL_LOG="$case_dir/tool.log" \
        DEPLOY_OBJDUMP_LOG="$case_dir/objdump.log" MSYSTEM= MSYSTEM_PREFIX="$prefix" bash build.sh) > "$case_dir/output.log" 2>&1
}
assert_file() { [ -f "$1" ] || { echo "FAIL: missing deployed file $1"; exit 1; }; }

prepare_case 6
# Старый комплект Qt5 и одноимённый plugin не должны удовлетворить проверку Qt6.
mkdir -p "$case_dir/build/platforms" "$prefix/share/qt5/plugins/platforms"
printf 'wrong Qt5 plugin\n' > "$prefix/share/qt5/plugins/platforms/qwindows.dll"
for component in Core Gui Network Widgets; do printf 'old Qt5\n' > "$case_dir/build/Qt5$component.dll"; done
run_case 6
for name in Qt6Gui.dll plugin-helper.dll jpeg-helper.dll webp-helper.dll imageformats/qjpeg.dll imageformats/qwebp.dll; do
    assert_file "$case_dir/build/$name"
done
cmp "$case_dir/build/platforms/qwindows.dll" "$prefix/share/qt6/plugins/platforms/qwindows.dll"
cmp "$case_dir/build/imageformats/qwebp.dll" "$prefix/share/qt6/plugins/imageformats/qwebp.dll"
[ "$(cat "$case_dir/tool.log")" = 'Qt6 deploy' ]
# Без изменений не запускаем ни objdump, ни windeployqt и не трогаем готовые DLL.
dll_stamp=$(stat -c '%y|%z' "$case_dir/build/Qt6Core.dll")
run_case 6
[ ! -s "$case_dir/objdump.log" ]
[ "$(cat "$case_dir/tool.log")" = 'Qt6 deploy' ]
[ "$dll_stamp" = "$(stat -c '%y|%z' "$case_dir/build/Qt6Core.dll")" ]
rg -q 'Runtime DLLs: copied=0 ' "$case_dir/output.log"
echo 'PASS: unchanged build uses cached imports and Qt deployment without copying DLLs'

# Перелинковка с прежними импортами затрагивает только EXE, но не запуск windeployqt.
printf '\nrelinked\n' >> "$case_dir/build/vibechat.exe"
run_case 6
[ "$(cat "$case_dir/objdump.log")" = "$case_dir/build/vibechat.exe" ]
[ "$(cat "$case_dir/tool.log")" = 'Qt6 deploy' ]
echo 'PASS: relink with unchanged imports scans only EXE'

# Наличие основных DLL не отменяет проверку транзитивных зависимостей.
rm "$case_dir/build/libpng.dll" "$case_dir/build/plugin-helper.dll"
run_case 6; assert_file "$case_dir/build/libpng.dll"; assert_file "$case_dir/build/plugin-helper.dll"
[ "$(wc -l < "$case_dir/objdump.log")" -eq 2 ]
[ "$(cat "$case_dir/tool.log")" = 'Qt6 deploy' ]
echo 'PASS: Qt6, Windows system DLLs (d3d9/SHCORE), matching platform/JPEG/WebP plugins and repeated dependency checks'

# Обновление транзитивной DLL добавляет новый импорт, даже когда копия уже существует.
printf 'DLL Name: updated-helper.dll\n' > "$prefix/bin/libpng.dll"
printf 'DLL Name: KERNEL32.dll\nrevision=1\n' > "$prefix/bin/updated-helper.dll"
run_case 6
cmp "$prefix/bin/libpng.dll" "$case_dir/build/libpng.dll"
assert_file "$case_dir/build/updated-helper.dll"
[ "$(wc -l < "$case_dir/objdump.log")" -eq 2 ]
echo 'PASS: changed transitive source refreshes existing DLL and deploys its new dependency'

# Размер и mtime не изменились: ctime всё равно инвалидирует копию и PE-кеш.
cp -p "$prefix/bin/updated-helper.dll" "$case_dir/timestamp-reference"
printf 'DLL Name: KERNEL32.dll\nrevision=2\n' > "$prefix/bin/updated-helper.dll"
touch -r "$case_dir/timestamp-reference" "$prefix/bin/updated-helper.dll"
run_case 6
cmp "$prefix/bin/updated-helper.dll" "$case_dir/build/updated-helper.dll"
[ "$(cat "$case_dir/objdump.log")" = "$case_dir/build/updated-helper.dll" ]
echo 'PASS: same-size source replacement with preserved mtime is detected'

# Повреждённую копию восстанавливаем из исходника, оборванный кеш пересчитываем.
printf 'damaged DLL\n' > "$case_dir/build/updated-helper.dll"
run_case 6
cmp "$prefix/bin/updated-helper.dll" "$case_dir/build/updated-helper.dll"
IFS= read -r cache_header < "$case_dir/build/.runtime-cache/imports/updated-helper.dll"
printf '%s\n' "$cache_header" > "$case_dir/build/.runtime-cache/imports/updated-helper.dll"
run_case 6
[ "$(cat "$case_dir/objdump.log")" = "$case_dir/build/updated-helper.dll" ]
echo 'PASS: damaged destination and incomplete import cache are repaired'

# Ошибка анализа нового файла не должна сохраняться как успешный пустой список импортов.
printf 'BROKEN\n' > "$prefix/bin/updated-helper.dll"
if run_case 6; then echo 'FAIL: invalid PE accepted'; exit 1; fi
rg -q 'cannot inspect .*updated-helper.dll.*exit=7' "$case_dir/output.log"
cp "$case_dir/timestamp-reference" "$prefix/bin/updated-helper.dll"
run_case 6
[ "$(cat "$case_dir/objdump.log")" = "$case_dir/build/updated-helper.dll" ]
echo 'PASS: failed PE inspection does not publish a successful cache entry'

# Изменение Qt запускает его раскладку; неизменившиеся DLL не пересканируются.
printf '\nupdated Qt\n' >> "$prefix/bin/Qt6Core.dll"
run_case 6
[ "$(wc -l < "$case_dir/tool.log")" -eq 2 ]
[ "$(cat "$case_dir/objdump.log")" = "$case_dir/build/Qt6Core.dll" ]
echo 'PASS: changed Qt source invalidates Qt deployment cache'

# Новый импорт EXE также должен обновлять входные данные windeployqt.
printf 'DLL Name: exe-helper.dll\n' >> "$case_dir/build/vibechat.exe"
printf 'DLL Name: KERNEL32.dll\n' > "$prefix/bin/exe-helper.dll"
run_case 6
[ "$(wc -l < "$case_dir/tool.log")" -eq 3 ]
assert_file "$case_dir/build/exe-helper.dll"
echo 'PASS: changed EXE imports refresh Qt deployment and resolve new DLLs'

# Правила раскладки входят в версию кеша, но изменение скрипта не требует копировать DLL.
printf '\n# changed deployment policy\n' >> "$case_dir/runtime_cache.sh"
run_case 6
[ "$(wc -l < "$case_dir/objdump.log")" -gt 2 ]
rg -q 'Runtime DLLs: copied=0 ' "$case_dir/output.log"
echo 'PASS: deployment policy change invalidates import cache'

# Кастомные кодеки имеют приоритет над UCRT64, но GCC runtime всегда берём из UCRT64.
ffmpeg_bin="$case_dir/ffmpeg/ffmpeg_build/install/bin"
mkdir -p "$ffmpeg_bin"
for name in avcodec-61.dll avformat-61.dll avutil-59.dll swscale-8.dll swresample-5.dll; do
    printf 'DLL Name: codec-helper.dll\n' > "$ffmpeg_bin/$name"
    printf 'wrong system codec\n' > "$prefix/bin/$name"
done
printf 'DLL Name: KERNEL32.dll\ncustom codec\n' > "$ffmpeg_bin/codec-helper.dll"
printf 'old compiler runtime\n' > "$ffmpeg_bin/libstdc++-6.dll"
run_case 6
cmp "$ffmpeg_bin/avcodec-61.dll" "$case_dir/build/avcodec-61.dll"
cmp "$prefix/bin/libstdc++-6.dll" "$case_dir/build/libstdc++-6.dll"
cmp "$ffmpeg_bin/codec-helper.dll" "$case_dir/build/codec-helper.dll"
run_case 6
[ ! -s "$case_dir/objdump.log" ]
rg -q 'Runtime DLLs: copied=0 ' "$case_dir/output.log"
printf '\nupdated custom codec\n' >> "$ffmpeg_bin/codec-helper.dll"
run_case 6
[ "$(cat "$case_dir/objdump.log")" = "$case_dir/build/codec-helper.dll" ]
cmp "$ffmpeg_bin/codec-helper.dll" "$case_dir/build/codec-helper.dll"
echo 'PASS: FFmpeg source priority, UCRT64 compiler runtime and incremental codec updates'

# Multimedia plugin динамический: импорт exe сам по себе не гарантирует его поставку.
mkdir -p "$prefix/share/qt6/plugins/multimedia"
printf 'DLL Name: Qt6Gui.dll\nDLL Name: AVRT.dll\n' > "$prefix/bin/Qt6Multimedia.dll"
printf 'DLL Name: Qt6Multimedia.dll\n' > "$prefix/bin/Qt6MultimediaWidgets.dll"
printf 'DLL Name: Qt6Multimedia.dll\nDLL Name: capture-helper.dll\nDLL Name: MFPlat.dll\n' > "$prefix/share/qt6/plugins/multimedia/ffmpegmediaplugin.dll"
printf 'DLL Name: KERNEL32.dll\n' > "$prefix/bin/capture-helper.dll"
run_case 6 1
for name in Qt6Multimedia.dll Qt6MultimediaWidgets.dll multimedia/ffmpegmediaplugin.dll capture-helper.dll; do
    assert_file "$case_dir/build/$name"
done
cmp "$case_dir/build/multimedia/ffmpegmediaplugin.dll" "$prefix/share/qt6/plugins/multimedia/ffmpegmediaplugin.dll"
if [ -f "$case_dir/build/AVRT.dll" ]; then echo 'FAIL: Windows system AVRT.dll deployed'; exit 1; fi
rm "$case_dir/build/multimedia/ffmpegmediaplugin.dll" "$case_dir/build/wintun.dll"
run_case 6 1
assert_file "$case_dir/build/multimedia/ffmpegmediaplugin.dll"
assert_file "$case_dir/build/wintun.dll"
[ "$(wc -l < "$case_dir/objdump.log")" -eq 2 ]
echo 'PASS: capture-enabled Qt6 deploys Multimedia, Widgets, FFmpeg plugin and plugin dependencies'

# Сбой подходящего инструмента не должен обходить обязательную проверку ручного комплекта.
cat > "$prefix/bin/windeployqt6.exe" <<'SH'
#!/bin/bash
if [ "$1" = --version ]; then echo 'windeployqt 6.10.0'; else exit 12; fi
SH
chmod +x "$prefix/bin/windeployqt6.exe"
run_case 6
rg -q 'windeployqt failed \(exit 12\)' "$case_dir/output.log"
echo 'PASS: failed matching deploy tool falls back to verified direct deployment'

# Нет подходящего deploy tool: копирование DLL/плагинов всё равно должно работать.
rm "$prefix/bin/windeployqt6.exe"
cat > "$prefix/bin/windeployqt.exe" <<'SH'
#!/bin/bash
if [ "$1" = --version ]; then echo 'windeployqt 5.15.0'; else exit 90; fi
SH
chmod +x "$prefix/bin/windeployqt.exe"
run_case 6
echo 'PASS: incorrect Qt5 deploy tool ignored for Qt6, direct deployment succeeds'

# Ошибка действительно отсутствующей сторонней DLL остаётся ошибкой сборки.
rm "$case_dir/build/jpeg-helper.dll" "$prefix/bin/jpeg-helper.dll"
if run_case 6; then echo 'FAIL: missing plugin dependency accepted'; exit 1; fi
rg -q 'requires jpeg-helper.dll' "$case_dir/output.log"
echo 'PASS: missing plugin dependency rejects incomplete deployment'

prepare_case 5; run_case 5
assert_file "$case_dir/build/Qt5Gui.dll"; [ "$(cat "$case_dir/tool.log")" = 'Qt5 deploy' ]
cmp "$case_dir/build/platforms/qwindows.dll" "$prefix/share/qt5/plugins/platforms/qwindows.dll"
assert_file "$case_dir/build/webp-helper.dll"
cmp "$case_dir/build/imageformats/qwebp.dll" "$prefix/share/qt5/plugins/imageformats/qwebp.dll"
echo 'PASS: configured Qt5 fallback deploys matching DLLs and plugins'
