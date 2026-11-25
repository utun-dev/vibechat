#!/bin/bash
# Общий модуль build.sh/package_win.sh; подключается через source для подготовки Windows runtime.
# Кеширует копирование DLL и разбор формата PE/импортов; изменения скриптов или objdump сбрасывают кеш.

# Обоим скриптам нужны одинаковые objdump и версия кеша. IMPORT_DUMP задаёт вызывающий код.
runtime_cache_init() {
    RUNTIME_CACHE="$BUILD_DIR/.runtime-cache"
    RUNTIME_COPIED=0 RUNTIME_CURRENT=0 RUNTIME_SCANNED=0 RUNTIME_HITS=0
    declare -gA RUNTIME_SOURCES=() RUNTIME_STAMPS=()
    OBJDUMP="${MSYSTEM_PREFIX:+$MSYSTEM_PREFIX/bin/}objdump"
    OBJDUMP=$(command -v "$OBJDUMP") || { log "  ERROR: objdump not found"; return 1; }
    CACHE_POLICY=$(sha256sum "$SRC_DIR/build.sh" "$SRC_DIR/package_win.sh" "$SRC_DIR/runtime_cache.sh" \
        "$SRC_DIR/compiler_runtime.sh" | sha256sum) || {
        log "  ERROR: cannot fingerprint runtime scripts"; return 1;
    }
    CACHE_POLICY+="|$(stat -Lc '%n|%s|%y|%z' -- "$OBJDUMP")" || { log "  ERROR: cannot stat $OBJDUMP"; return 1; }
}

runtime_cache_write() {
    local cache_file="$1" cache_data="$2"
    if ! mkdir -p "${cache_file%/*}" || ! printf '%s\n' "$cache_data" > "$cache_file.tmp" ||
        ! mv -f "$cache_file.tmp" "$cache_file"; then
        log "  ERROR: cannot save runtime cache: $cache_file"
        return 1
    fi
}

# Источник входит в подпись: одинаковые имена DLL из разных комплектов не взаимозаменяемы.
# ctime позволяет заметить замену файла с сохранёнными размером и mtime.
runtime_copy() {
    local copy_src="$1" copy_dst="$2" copy_stamp copy_cached="" copy_rc
    local copy_cache="$RUNTIME_CACHE/copies/${2#"$BUILD_DIR/"}"
    RUNTIME_SOURCES["${copy_dst,,}"]="$copy_src"
    if [ -f "$copy_dst" ]; then
        copy_stamp=$(stat -Lc '%n|%s|%y|%z' -- "$copy_src" "$copy_dst") || {
            log "  ERROR: cannot stat runtime pair: $copy_src -> $copy_dst"; return 1;
        }
        RUNTIME_STAMPS["${copy_dst,,}"]=${copy_stamp#*$'\n'}
        [ ! -f "$copy_cache" ] || copy_cached=$(< "$copy_cache")
        if [ "$copy_cached" = "$copy_stamp" ]; then
            RUNTIME_CURRENT=$((RUNTIME_CURRENT + 1))
            return 0
        fi
        if cmp -s "$copy_src" "$copy_dst"; then
            runtime_cache_write "$copy_cache" "$copy_stamp" || return 1
            RUNTIME_CURRENT=$((RUNTIME_CURRENT + 1))
            return 0
        else
            copy_rc=$?
            if [ "$copy_rc" -ne 1 ]; then
                log "  ERROR: cannot compare $copy_src -> $copy_dst (exit=$copy_rc)"; return 1;
            fi
        fi
    fi
    if ! mkdir -p "${copy_dst%/*}" || ! cp "$copy_src" "$copy_dst"; then
        log "  ERROR: cannot deploy $copy_src -> $copy_dst"; return 1;
    fi
    copy_stamp=$(stat -Lc '%n|%s|%y|%z' -- "$copy_src" "$copy_dst") || {
        log "  ERROR: cannot stat deployed runtime: $copy_dst"; return 1;
    }
    RUNTIME_STAMPS["${copy_dst,,}"]=${copy_stamp#*$'\n'}
    runtime_cache_write "$copy_cache" "$copy_stamp" || return 1
    RUNTIME_COPIED=$((RUNTIME_COPIED + 1))
    log "  Updated: ${copy_dst#"$BUILD_DIR/"} (from $copy_src)"
}

# Возвращает binary_format, binary_stamp и import_list; вызывать без subshell, чтобы сохранить счётчики.
# Завершающая строка отличает полноценную запись с пустыми импортами от оборванной.
runtime_inspect() {
    local import_src="$1" import_stamp import_cached="" import_header import_started=$SECONDS import_rc inspection
    local import_cache="$RUNTIME_CACHE/imports/${1#"$BUILD_DIR/"}"
    # runtime_copy уже проверил метаданные этой копии; не запускаем stat второй раз.
    import_stamp=${RUNTIME_STAMPS[${import_src,,}]:-}
    if [ -z "$import_stamp" ]; then
        import_stamp=$(stat -Lc '%n|%s|%y|%z' -- "$import_src") || {
            log "  ERROR: cannot stat PE file: $import_src"; return 1;
        }
    fi
    binary_stamp="$import_stamp"
    import_header="$CACHE_POLICY|$import_stamp"
    [ ! -f "$import_cache" ] || import_cached=$(< "$import_cache")
    if [[ "$import_cached" == "$import_header"$'\nFORMAT '*$'\n'*$'\nEND' ]]; then
        inspection=${import_cached#*$'\nFORMAT '}
        binary_format=${inspection%%$'\n'*}
        import_list=${inspection#*$'\n'}
        import_list=${import_list%$'\nEND'}
        RUNTIME_HITS=$((RUNTIME_HITS + 1))
        return 0
    fi
    "$OBJDUMP" -p "$import_src" > "$IMPORT_DUMP" 2>&1 || {
        import_rc=$?
        log "  ERROR: cannot inspect $import_src (exit=$import_rc, elapsed=$((SECONDS - import_started))s): $(cat "$IMPORT_DUMP")"
        return 1
    }
    # objdump -p уже выводит file format; отдельный objdump -f не нужен.
    inspection=$(awk '{sub(/\r$/, "")} /file format/ {format=$NF} /DLL Name:/ {imports=imports $3 "\n"}
        END {if (format == "") exit 1; printf "%s\n%s", format, imports}' "$IMPORT_DUMP") || {
        log "  ERROR: cannot parse PE format/imports: $import_src"; return 1;
    }
    binary_format=${inspection%%$'\n'*}
    import_list=""
    [[ "$inspection" != *$'\n'* ]] || import_list=${inspection#*$'\n'}
    import_stamp=$(stat -Lc '%n|%s|%y|%z' -- "$import_src") || {
        log "  ERROR: cannot stat inspected PE file: $import_src"; return 1;
    }
    if [ "$binary_stamp" != "$import_stamp" ]; then
        log "  ERROR: PE file changed during inspection: $import_src; rebuild and retry"; return 1;
    fi
    runtime_cache_write "$import_cache" "$import_header"$'\nFORMAT '"$binary_format"$'\n'"$import_list"$'\nEND' || return 1
    RUNTIME_SCANNED=$((RUNTIME_SCANNED + 1))
}
