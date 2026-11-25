#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

VERSION=$(awk -F'"' '/^[[:space:]]*versionName[[:space:]]*=/ { print $2; exit }' app/build.gradle.kts)
if [[ -z "$VERSION" ]]; then
    echo "ERROR: Cannot read versionName from app/build.gradle.kts." >&2
    exit 1
fi

./build.sh --release noinstall

APK="app/build/outputs/apk/release/app-release.apk"
RELEASE_APK="vibe_${VERSION}.apk"
HTML="../../html/index.html"
APK_URL="app/$RELEASE_APK"
cp "$APK" "$RELEASE_APK"
echo "Release APK: $PWD/$RELEASE_APK"

HTML_TMP=$(mktemp)
trap 'rm -f "$HTML_TMP"' EXIT
if ! awk -v apk_url="$APK_URL" '
    /<a class="button button-outline" href="[^"]*">Скачать APK<\/a>/ {
        sub(/href="[^"]*"/, "href=\"" apk_url "\"")
        updated = 1
    }
    { print }
    END { if (!updated) exit 1 }
' "$HTML" > "$HTML_TMP"; then
    echo "ERROR: APK download link not found in $HTML." >&2
    exit 1
fi
cp "$HTML_TMP" "$HTML"
echo "APK link: $HTML -> $APK_URL"

scp "$RELEASE_APK" "vc@vc:/home/vc/www/app/"
scp "$HTML" "vc@vc:/home/vc/www/index.html"
echo "Published: vc@vc:/home/vc/www/$APK_URL"
