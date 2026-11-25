#!/bin/bash
# Вспомогательные функции для Windows-сборки; подключается через source.
# Обновляет DLL компилятора в каталоге сборки из установленного UCRT64 перед запуском CMake.

# Общие DLL GCC и его внутренних компиляторов должны соответствовать установленному UCRT64.
chatgui_is_compiler_runtime_dll() {
    case "${1,,}" in
        libgcc_s_*.dll|libstdc++-6.dll|libwinpthread-1.dll|libgmp-*.dll|libisl-*.dll|libmpc-*.dll|libmpfr-*.dll|zlib1.dll|libzstd.dll)
            return 0 ;;
        *) return 1 ;;
    esac
}

# Обновляем уже разложенные DLL до запуска CMake: Windows ищет их в рабочем каталоге cc1.
chatgui_refresh_compiler_runtime() {
    local runtime_build="$1" runtime_bin="$2" runtime_dst runtime_name runtime_src runtime_rc
    local runtime_updated=0 runtime_current=0
    for runtime_dst in "$runtime_build/"*.dll; do
        [ -f "$runtime_dst" ] || continue
        runtime_name="${runtime_dst##*/}"
        chatgui_is_compiler_runtime_dll "$runtime_name" || continue
        runtime_src="$runtime_bin/$runtime_name"
        if [ ! -f "$runtime_src" ]; then
            printf '[ERROR] Compiler runtime source missing: %s (local copy: %s)\n' "$runtime_src" "$runtime_dst"
            return 1
        fi
        if cmp -s "$runtime_src" "$runtime_dst"; then
            runtime_current=$((runtime_current + 1))
            continue
        else
            runtime_rc=$?
            if [ "$runtime_rc" -ne 1 ]; then
                printf '[ERROR] Cannot compare compiler runtime DLL: %s, exit code: %d\n' "$runtime_dst" "$runtime_rc"
                return "$runtime_rc"
            fi
        fi
        cp "$runtime_src" "$runtime_dst" || {
            printf '[ERROR] Cannot refresh compiler runtime DLL: %s -> %s\n' "$runtime_src" "$runtime_dst"
            return 1
        }
        printf '[INFO] Refreshed compiler runtime DLL: %s -> %s\n' "$runtime_src" "$runtime_dst"
        runtime_updated=$((runtime_updated + 1))
    done
    printf '[INFO] Compiler runtime DLLs: updated=%d current=%d\n' "$runtime_updated" "$runtime_current"
}
