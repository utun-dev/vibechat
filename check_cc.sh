#!/bin/bash
set -uo pipefail

# Проверяем базовую компиляцию, затем Opus отдельно и через существующую CMake-сборку.
probe_cc="${UTUN_PROBE_CC:-/ucrt64/bin/cc.exe}"
printf '[INFO] MSYSTEM=%s\n[INFO] PATH=%s\n[INFO] Compiler=%s\n' "${MSYSTEM:-unset}" "$PATH" "$probe_cc"
if [ ! -x "$probe_cc" ]; then
    printf '[ERROR] Compiler is missing or not executable: %s\n' "$probe_cc"
    exit 1
fi

probe_dir=$(mktemp -d "${TMPDIR:-/tmp}/utun-cc-probe.XXXXXX") || {
    printf '[ERROR] Cannot create the temporary probe directory.\n'
    exit 1
}
trap 'rm -rf -- "$probe_dir" || printf "[WARN] Cannot remove probe directory: %s\n" "$probe_dir"' EXIT
printf '[INFO] Temporary directory: %s\n' "$probe_dir"

# Каждый этап сохраняет собственный код завершения, включая молчаливые сбои.
probe_step() {
    local step="$1" result
    shift
    printf '\n[INFO] %s\n[INFO] Directory: %s\n[INFO] Command:' "$step" "$PWD"
    printf ' %q' "$@"
    printf '\n'
    "$@"
    result=$?
    printf '[INFO] %s exit code: %d\n' "$step" "$result"
    if [ "$result" -ne 0 ]; then
        printf '[ERROR] Failed stage: %s\n' "$step"
    fi
    return "$result"
}

printf '#include <stdio.h>\nint main(void) { puts("utun compiler probe OK"); return 0; }\n' > "$probe_dir/probe.c" || {
    printf '[ERROR] Cannot write the probe source: %s/probe.c\n' "$probe_dir"
    exit 1
}
probe_step 'Compiler version' "$probe_cc" --version || exit $?
probe_step 'Compile' "$probe_cc" -v -std=c99 -c "$probe_dir/probe.c" -o "$probe_dir/probe.o" || exit $?
probe_step 'Link' "$probe_cc" -v "$probe_dir/probe.o" -o "$probe_dir/probe.exe" || exit $?
probe_step 'Run' "$probe_dir/probe.exe" || exit $?
printf '\n[SUCCESS] Compile, link and run completed.\n'

probe_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd) || {
    printf '[ERROR] Cannot locate the repository directory.\n'
    exit 1
}
probe_opus="$probe_root/lib/libopus"
probe_includes=()
for probe_include in '' include celt silk silk/float silk/fixed src; do
    probe_includes+=("-I$probe_opus/$probe_include")
done
for probe_source in laplace bands; do
    probe_step "Compile Opus $probe_source.c" "$probe_cc" -v -DOPUS_BUILD -DUSE_ALLOCA -DVAR_ARRAYS "${probe_includes[@]}" \
        -O3 -DNDEBUG -std=gnu99 -MD -MF "$probe_dir/$probe_source.d" \
        -c "$probe_opus/celt/$probe_source.c" -o "$probe_dir/$probe_source.o" || exit $?
done

probe_build="$probe_root/tools/chatgui/build"
if [ ! -f "$probe_build/CMakeCache.txt" ]; then
    printf '[ERROR] Existing ChatGUI build cache not found: %s/CMakeCache.txt\n' "$probe_build"
    exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
    printf '[ERROR] cmake is not available in PATH.\n'
    exit 1
fi
probe_step 'CMake cache settings' sed -n -E \
    '/^CMAKE_(C_COMPILER|C_COMPILER_LAUNCHER|C_FLAGS(_(DEBUG|RELEASE|MINSIZEREL|RELWITHDEBINFO))?|MAKE_PROGRAM|GENERATOR|BUILD_TYPE):/p' \
    "$probe_build/CMakeCache.txt" || exit $?

# Повторяем Windows-команду без преобразования аргументов MSYS2; добавляем только GCC -v.
if [ "${MSYSTEM:-}" = UCRT64 ]; then
    probe_ninja=$(sed -n 's/^CMAKE_MAKE_PROGRAM:[^=]*=//p' "$probe_build/CMakeCache.txt")
    probe_ninja=${probe_ninja%$'\r'}
    probe_ninja=$(cygpath -u "$probe_ninja") || exit $?
    "$probe_ninja" -C "$probe_build" -t commands opus_internal > "$probe_dir/ninja-commands.txt"
    probe_result=$?
    if [ "$probe_result" -ne 0 ]; then
        cat "$probe_dir/ninja-commands.txt"
        printf '[ERROR] Cannot read Ninja commands, exit code: %d\n' "$probe_result"
        exit "$probe_result"
    fi
    probe_command=''
    while IFS= read -r probe_line; do
        probe_line=${probe_line%$'\r'}
        case "$probe_line" in
            *' -DOPUS_BUILD '*' -c '*'/celt/bands.c'|*' -DOPUS_BUILD '*' -c '*'/celt/bands.c"')
                probe_command="$probe_line"; break ;;
        esac
    done < "$probe_dir/ninja-commands.txt"
    if [ -z "$probe_command" ]; then
        printf '[ERROR] Cannot find the Opus bands.c compile command in Ninja.\n'
        exit 1
    fi
    probe_command=${probe_command/ -DOPUS_BUILD/ -v -DOPUS_BUILD}
    printf '[INFO] Native command: %s\n' "$probe_command"
    printf '@echo off\r\n%s\r\nexit /b %%errorlevel%%\r\n' "$probe_command" > "$probe_dir/replay.cmd" || {
        printf '[ERROR] Cannot write the native compiler replay script.\n'
        exit 1
    }
    probe_cmd=$(cygpath -u "${COMSPEC:-C:/Windows/System32/cmd.exe}") || exit $?
    probe_replay=$(cygpath -w "$probe_dir/replay.cmd") || exit $?

    # CMD сохраняет полный Windows-код завершения cc1; Bash оставляет только младший байт.
    probe_cc1=$("$probe_cc" -print-prog-name=cc1) || { printf '[ERROR] Cannot locate cc1.\n'; exit 1; }
    probe_cc1=${probe_cc1%$'\r'}
    probe_cc1=$(cygpath -u "$probe_cc1") || { printf '[ERROR] Cannot resolve the cc1 path.\n'; exit 1; }
    probe_cc1_native=$(cygpath -w "$probe_cc1") || { printf '[ERROR] Cannot resolve the native cc1 path.\n'; exit 1; }
    printf '@echo off\r\n"%s" --version\r\nset "UTUN_CC1_RESULT=%%errorlevel%%"\r\n' "$probe_cc1_native" \
        > "$probe_dir/cc1.cmd" || { printf '[ERROR] Cannot write the cc1 probe script.\n'; exit 1; }
    printf 'echo [INFO] cc1 Windows exit code: %%UTUN_CC1_RESULT%%\r\nexit /b %%UTUN_CC1_RESULT%%\r\n' \
        >> "$probe_dir/cc1.cmd" || { printf '[ERROR] Cannot finish the cc1 probe script.\n'; exit 1; }
    probe_cc1_script=$(cygpath -w "$probe_dir/cc1.cmd") || { printf '[ERROR] Cannot resolve the cc1 script path.\n'; exit 1; }
    for probe_cwd in "$probe_root" "$probe_build"; do
        cd -- "$probe_cwd" || { printf '[ERROR] Cannot open probe directory: %s\n' "$probe_cwd"; exit 1; }
        MSYS2_ARG_CONV_EXCL='*' probe_step 'Launch cc1 before runtime refresh' "$probe_cmd" /d /c call "$probe_cc1_script"
        # При ошибке продолжаем сравнение каталогов и DLL, затем повторяем исходный сбой.
    done
    cd -- "$probe_root" || { printf '[ERROR] Cannot open the repository directory.\n'; exit 1; }
    probe_objdump="${probe_cc%/*}/objdump.exe"
    "$probe_objdump" -p "$probe_cc1" > "$probe_dir/cc1-imports.txt" || {
        printf '[ERROR] Cannot inspect cc1 imports.\n'; exit 1;
    }
    printf '\n[INFO] cc1 DLL imports:\n'
    sed -n '/DLL Name:/p' "$probe_dir/cc1-imports.txt" || { printf '[ERROR] Cannot read cc1 imports.\n'; exit 1; }
    source "$probe_root/tools/chatgui/compiler_runtime.sh"
    for probe_dll in "$probe_build/"*.dll; do
        [ -f "$probe_dll" ] || continue
        chatgui_is_compiler_runtime_dll "${probe_dll##*/}" || continue
        probe_system_dll="${probe_cc%/*}/${probe_dll##*/}"
        [ -f "$probe_system_dll" ] || continue
        cmp -s "$probe_dll" "$probe_system_dll"
        probe_result=$?
        case "$probe_result" in
            0) printf '[INFO] DLL matches UCRT64: %s\n' "${probe_dll##*/}" ;;
            1) printf '[WARN] DLL differs from UCRT64: %s\n' "${probe_dll##*/}"
               probe_step 'DLL hashes' sha256sum "$probe_dll" "$probe_system_dll" || exit $? ;;
            *) printf '[ERROR] Cannot compare DLL: %s, exit code: %d\n' "$probe_dll" "$probe_result"; exit "$probe_result" ;;
        esac
    done
    probe_step 'Refresh compiler runtime DLLs' chatgui_refresh_compiler_runtime "$probe_build" "${probe_cc%/*}" || exit $?
    cd -- "$probe_build" || { printf '[ERROR] Cannot open the ChatGUI build directory.\n'; exit 1; }
    MSYS2_ARG_CONV_EXCL='*' probe_step 'Launch cc1 after runtime refresh' "$probe_cmd" /d /c call "$probe_cc1_script" || exit $?
    MSYS2_ARG_CONV_EXCL='*' probe_step 'Replay Ninja bands.c command with GCC verbose output' \
        "$probe_cmd" /d /c call "$probe_replay" || exit $?
fi
probe_step 'Build Opus with CMake' cmake --build "$probe_build" --target opus_internal --parallel 1 --verbose || exit $?
printf '\n[SUCCESS] Standalone Opus compilation and CMake Opus build completed.\n'
