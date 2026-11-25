#!/usr/bin/env bash
# Устанавливает или обновляет VibeChat в ~/.local/opt/vibechat и добавляет значок в пользовательское меню.
# Проверяет зависимости, откатывает замену при ошибке и сохраняет профиль; --check выполняет только проверку.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")" && pwd)
fail() { echo "ERROR: $*" >&2; exit 1; }
[[ $# = 0 || ( $# = 1 && $1 = --check ) ]] || fail 'Usage: bash install.sh [--check]'
bash "$ROOT/check.sh"
[[ $# = 0 ]] || exit 0
[[ -n ${HOME:-} && $HOME = /* && $HOME != / ]] || fail 'HOME must be an absolute user directory'
app="$HOME/.local/opt/vibechat"
data=${XDG_DATA_HOME:-$HOME/.local/share}
[[ $data = /* && $data != *$'\n'* && $app != *$'\n'* ]] || fail 'Installation paths must be absolute and contain no newlines'
desktop="$data/applications/org.utun.vibechat.desktop"
[[ ! -L $app ]] || fail "Installation directory must not be a symlink: $app"
[[ ! -e $app || -f $app/.vibechat-user-install ]] || fail "Refusing to replace an unmanaged directory: $app"
[[ ! -e $desktop || -f $app/.vibechat-user-install ]] || fail "Refusing to overwrite an existing launcher: $desktop"
if [[ -f $app/desktop-path.txt ]]; then
    IFS= read -r previous_desktop < "$app/desktop-path.txt"
    [[ $previous_desktop = "$desktop" ]] || fail 'XDG_DATA_HOME changed; uninstall the previous installation first'
fi
stage=''; backup=''; launcher=''; replaced=0
cleanup() {
    status=$?
    trap - EXIT
    if (( status != 0 )); then
        echo "ERROR: installation failed (exit=$status); restoring previous application" >&2
        if [[ $replaced = 1 ]]; then rm -rf -- "$app"; fi
        if [[ -n $backup && -d $backup/app ]]; then mv -- "$backup/app" "$app"; fi
    fi
    [[ -z $stage ]] || rm -rf -- "$stage"
    [[ -z $launcher ]] || rm -f -- "$launcher"
    [[ -z $backup ]] || rm -rf -- "$backup"
    exit "$status"
}
trap cleanup EXIT
mkdir -p -- "${app%/*}" "${desktop%/*}"
stage=$(mktemp -d "${app%/*}/.vibechat-stage-XXXXXX")
# Только файлы выпуска: профиль или посторонние файлы из распакованного каталога не переносим.
for item in bin licenses icon.png check.sh dependencies.txt plugins.txt uninstall.sh .vibechat-user-install; do
    cp -a -- "$ROOT/$item" "$stage/"
done
printf '%s\n' "$desktop" > "$stage/desktop-path.txt"
launcher=$(mktemp "${desktop%/*}/.vibechat-desktop-XXXXXX")
# Desktop Entry имеет собственное экранирование; % в Exec обозначает field code.
exec_path="$app/bin/vibechat"
exec_path=${exec_path//\\/\\\\}; exec_path=${exec_path//\"/\\\"}; exec_path=${exec_path//\$/\\$}; exec_path=${exec_path//\`/\\\`}
exec_path=${exec_path//%/%%}
# Сначала экранирование аргумента Exec, затем экранирование строки Desktop Entry.
exec_path=${exec_path//\\/\\\\}
icon_path=${app//\\/\\\\}
printf 'Exec=/usr/bin/env "%s" --minimized\n' "$exec_path" > "$stage/autostart-exec.txt"
cat > "$launcher" <<EOF
[Desktop Entry]
Type=Application
Name=VibeChat
Comment=Peer-to-peer messenger
Exec=/usr/bin/env "$exec_path" -- %u
Icon=$icon_path/icon.png
Terminal=false
Categories=Network;InstantMessaging;
MimeType=x-scheme-handler/utun;
StartupWMClass=vibechat
EOF
chmod 644 "$launcher"
if [[ -e $app ]]; then
    backup=$(mktemp -d "${app%/*}/.vibechat-backup-XXXXXX")
    mv -- "$app" "$backup/app"
fi
replaced=1
mv -- "$stage" "$app"
stage=''
mv -f -- "$launcher" "$desktop"
launcher=''
echo "INFO: VibeChat installed: $app; launcher: $desktop"
echo "INFO: profile preserved; uninstall with: bash \"$app/uninstall.sh\""
if command -v update-desktop-database >/dev/null; then
    update-desktop-database "${desktop%/*}" || echo 'WARN: desktop menu cache refresh failed' >&2
fi
if command -v xdg-mime >/dev/null; then
    xdg-mime default org.utun.vibechat.desktop x-scheme-handler/utun || echo 'WARN: utun URL handler registration failed' >&2
fi
