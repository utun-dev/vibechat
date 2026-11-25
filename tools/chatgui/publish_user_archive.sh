#!/usr/bin/env bash
# Собирает пользовательский установщик Ubuntu 26.04 x64 и обновляет ссылку на архив в html/index.html.
# Публикует архив и HTML через scp на vc@vc в /home/vc/www; аргументы передаются скрипту сборки.
set -euo pipefail
cd "$(dirname "$0")"
HTML_TMP=''
trap 'rc=$?; [[ -z "$HTML_TMP" ]] || rm -f -- "$HTML_TMP";
    if [[ $rc != 0 ]]; then echo "ERROR: user installer publication failed (exit=$rc)" >&2; fi' EXIT

./build_nix.sh --user-archive "$@"

read -r VERSION < build-user/chatgui_version.txt
[[ $VERSION =~ ^[0-9]+(\.[0-9]+)+$ ]] || { echo "ERROR: invalid release version: $VERSION" >&2; exit 1; }
ARCHIVE="build-user/installer/vibechat-$VERSION-ubuntu-26.04-amd64.tar.gz"
ARCHIVE_NAME="vibe-$VERSION-ubuntu-26.04-amd64.tar.gz"
HTML="../../html/index.html"
ARCHIVE_URL="app/$ARCHIVE_NAME"
[[ -s $ARCHIVE ]] || { echo "ERROR: user installer archive not found after build: $ARCHIVE" >&2; exit 1; }
echo "INFO: user installer ready for publication: $PWD/$ARCHIVE"

HTML_TMP=$(mktemp)
if ! awk -v archive_url="$ARCHIVE_URL" '
    /<a[^>]*id="download-linux-user"[^>]*>/ {
        if (sub(/href="[^"]*"/, "href=\"" archive_url "\"")) updated++
    }
    { print }
    END { if (updated != 1) exit 1 }
' "$HTML" > "$HTML_TMP"; then
    echo "ERROR: expected one download-linux-user link with href in $HTML" >&2
    exit 1
fi
cp "$HTML_TMP" "$HTML"
echo "INFO: user installer link: $HTML -> $ARCHIVE_URL"

scp "$ARCHIVE" "vc@vc:/home/vc/www/$ARCHIVE_URL"
echo "INFO: user installer uploaded; publishing download page"
scp "$HTML" "vc@vc:/home/vc/www/index.html"
echo "INFO: published user installer: vc@vc:/home/vc/www/$ARCHIVE_URL"
