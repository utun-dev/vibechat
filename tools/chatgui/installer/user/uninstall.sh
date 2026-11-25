#!/usr/bin/env bash
# Удаляет пользовательскую установку VibeChat, её значок в меню и соответствующую запись автозапуска.
# Запускается из ~/.local/opt/vibechat, сохраняет профиль и не требует sudo.
set -euo pipefail
trap 'echo "ERROR: uninstall failed at line $LINENO" >&2' ERR
app=$(cd -- "$(dirname -- "$0")" && pwd)
if [[ $app != "$HOME/.local/opt/vibechat" || ! -f $app/.vibechat-user-install || ! -f $app/desktop-path.txt ]]; then
    echo 'ERROR: run uninstall.sh from the installed VibeChat directory' >&2; exit 1
fi
IFS= read -r desktop < "$app/desktop-path.txt"
[[ $desktop = /*/applications/org.utun.vibechat.desktop ]] || { echo 'ERROR: invalid launcher path' >&2; exit 1; }
# GUI включает автозапуск при первом старте; удаляем только запись именно этого executable.
autostart="${XDG_CONFIG_HOME:-$HOME/.config}/autostart/org.utun.vibechat.desktop"
if [[ -f $autostart && -f $app/autostart-exec.txt ]] && grep -Fqx -f "$app/autostart-exec.txt" "$autostart"; then
    rm -f -- "$autostart"
    echo "INFO: removed application autostart: $autostart"
fi
rm -f -- "$desktop"
rm -rf -- "$app"
echo 'INFO: VibeChat removed; user profile preserved'
if command -v update-desktop-database >/dev/null; then
    update-desktop-database "${desktop%/*}" || echo 'WARN: desktop menu cache refresh failed' >&2
fi
