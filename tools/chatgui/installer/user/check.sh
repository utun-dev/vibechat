#!/usr/bin/env bash
# Проверяет зависимости пользовательского архива VibeChat для Ubuntu 26.04 x64: версии пакетов, ELF и Qt-плагины.
# При нехватке пакетов выводит команду APT; ничего не устанавливает и не меняет установленное приложение.
set -euo pipefail
export LC_ALL=C
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fail() { echo "ERROR: $*" >&2; exit 1; }
trap 'echo "ERROR: dependency check failed at line $LINENO" >&2' ERR
[[ $(uname -s) = Linux && $(uname -m) = x86_64 ]] || fail 'Ubuntu 26.04 x64 is required'
source /etc/os-release
[[ ${ID:-} = ubuntu && ${VERSION_ID:-} = 26.04 ]] || fail 'This archive requires Ubuntu 26.04; APT cannot fix an incompatible OS'
for tool in dpkg dpkg-query ldd; do command -v "$tool" >/dev/null || fail "Required system tool missing: $tool"; done
[[ -f "$ROOT/dependencies.txt" && -f "$ROOT/plugins.txt" ]] || fail 'Archive dependency manifest is missing'
missing=()
while IFS= read -r clause; do
    [[ -n $clause ]] || continue
    satisfied=0
    IFS='|' read -ra alternatives <<< "$clause"
    for alternative in "${alternatives[@]}"; do
        if [[ $alternative =~ ^[[:space:]]*([a-z0-9][a-z0-9+.:_-]*)([[:space:]]+\((\>\=|\=|\<\=|\>\>|\<\<)[[:space:]]+([^\)]+)\))?[[:space:]]*$ ]]; then
            package=${BASH_REMATCH[1]}; operator=${BASH_REMATCH[3]}; required=${BASH_REMATCH[4]}
        else
            fail "Invalid dependency expression: $alternative"
        fi
        status=$(dpkg-query -W '-f=${db:Status-Status} ${Version}' "$package" 2>/dev/null || true)
        if [[ $status = installed\ * ]]; then
            version=${status#installed }
            if [[ -z $operator ]] || dpkg --compare-versions "$version" "$operator" "$required"; then satisfied=1; break; fi
        fi
    done
    if [[ $satisfied = 0 ]]; then
        echo "ERROR: missing or outdated dependency: $clause" >&2
        missing+=("${alternatives[0]}")
    fi
done < "$ROOT/dependencies.txt"
if (( ${#missing[@]} )); then
    echo 'INFO: install/upgrade these packages, then run install.sh again:'
    printf 'sudo apt update && sudo apt install'
    for expression in "${missing[@]}"; do
        read -r package _ <<< "$expression"
        printf ' %q' "$package"
    done
    printf '\n'
    exit 1
fi
objects=("$ROOT/bin/vibechat")
while IFS= read -r plugin; do
    [[ $plugin = /usr/lib/* && -f $plugin ]] || fail "Required Qt plugin missing: $plugin; reinstall its package listed in dependencies.txt"
    objects+=("$plugin")
done < "$ROOT/plugins.txt"
for object in "${objects[@]}"; do
    if ! diagnostic=$(env -u LD_LIBRARY_PATH -u LD_PRELOAD ldd "$object" 2>&1) || [[ $diagnostic = *'not found'* ]]; then
        fail "Runtime is broken for $object: $diagnostic; reinstall the affected package"
    fi
done
echo "INFO: Ubuntu 26.04 x64 runtime ready: package versions and ${#objects[@]} ELF/plugin files checked"
