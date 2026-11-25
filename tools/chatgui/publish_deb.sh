#!/usr/bin/env bash
# Собирает DEB через build_nix.sh --deb, копирует пакет выпуска и обновляет ссылку в html/index.html.
# Публикует пакет и HTML через scp на vc@vc в /home/vc/www; аргументы передаются скрипту сборки.
set -euo pipefail
cd "$(dirname "$0")"

./build_nix.sh --deb "$@"

read -r VERSION < build-deb/chatgui_version.txt
ARCHITECTURE=$(dpkg --print-architecture)
DEB="build-deb/installer/vibechat_${VERSION}_${ARCHITECTURE}.deb"
RELEASE_DEB="vibe_${VERSION}.deb"
HTML="../../html/index.html"
DEB_URL="app/$RELEASE_DEB"
if [[ ! -f "$DEB" ]]; then
    echo "ERROR: DEB package not found after build: $DEB" >&2
    exit 1
fi
cp "$DEB" "$RELEASE_DEB"
echo "Release DEB: $PWD/$RELEASE_DEB"

HTML_TMP=$(mktemp)
trap 'rm -f "$HTML_TMP"' EXIT
if ! awk -v deb_url="$DEB_URL" '
    /<a class="button button-outline" href="[^"]*">Скачать DEB<\/a>/ {
        sub(/href="[^"]*"/, "href=\"" deb_url "\"")
        updated = 1
    }
    { print }
    END { if (!updated) exit 1 }
' "$HTML" > "$HTML_TMP"; then
    echo "ERROR: DEB download link not found in $HTML." >&2
    exit 1
fi
cp "$HTML_TMP" "$HTML"
echo "DEB link: $HTML -> $DEB_URL"

scp "$RELEASE_DEB" "vc@vc:/home/vc/www/app/"
scp "$HTML" "vc@vc:/home/vc/www/index.html"
echo "Published: vc@vc:/home/vc/www/$DEB_URL"
