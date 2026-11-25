#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -f ../../proxy.sh ]; then
    . ../../proxy.sh || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi
BUILD_TYPE=Debug
NOINSTALL=0
DEVICE=""
for arg in "$@"; do
    case "$arg" in
        --release) BUILD_TYPE=Release ;;
        install|--install) NOINSTALL=0 ;;
        noinstall|--no-install) NOINSTALL=1 ;;
        --help|-h)
            echo "Usage: $0 [--release] [install | noinstall | DEVICE_SERIAL]"
            echo "Default: clean debug build and install on all connected devices."
            echo "Release: create the key once with ./create_signing_key.sh; then use --release noinstall."
            exit 0
            ;;
        -*) echo "ERROR: Unknown option: $arg. Use --help." >&2; exit 1 ;;
        *)
            if [[ -n "$DEVICE" ]]; then
                echo "ERROR: Only one device serial may be specified." >&2
                exit 1
            fi
            DEVICE="$arg"
            ;;
    esac
done
if [[ "$NOINSTALL" == 1 && -n "$DEVICE" ]]; then
    echo "ERROR: Specify either noinstall or a device serial." >&2
    exit 1
fi
if [[ "$BUILD_TYPE" == Release && ! -f signing/keystore.properties ]]; then
    echo "ERROR: Release signing settings missing. Run ./create_signing_key.sh first." >&2
    exit 1
fi
command -v python3 >/dev/null || { echo "ERROR: Python 3 is required; run ./prepare_build.sh first." >&2; exit 1; }
echo "Preparing Android SDK and native dependencies..."
bash ./prepare_build.sh --if-needed || {
    echo "ERROR: Android preparation failed; APK build not started." >&2; exit 1;
}
# local.properties — тот же источник SDK, который использует Gradle; не подставляем путь разработчика.
SDK_PATH=$(PYTHONPATH="$PWD/.." python3 -c '
import os
from pathlib import Path
from prepare_android import properties
print(properties(Path("local.properties")).get("sdk.dir") or os.getenv("ANDROID_HOME") or os.getenv("ANDROID_SDK_ROOT") or "")
')
[[ -n "$SDK_PATH" && -d "$SDK_PATH" ]] || { echo "ERROR: Android SDK not configured; run ./prepare_build.sh first." >&2; exit 1; }
export ANDROID_HOME="$SDK_PATH" ANDROID_SDK_ROOT="$SDK_PATH"
export PATH="$SDK_PATH/platform-tools:$PATH"
if [[ "$NOINSTALL" == 0 ]] && ! command -v adb >/dev/null; then
    echo "ERROR: adb not found; run ./prepare_build.sh to install Android platform-tools." >&2; exit 1
fi
VARIANT="${BUILD_TYPE,,}"
APK="app/build/outputs/apk/$VARIANT/app-$VARIANT.apk"
trap 'echo "ERROR: Android build/install failed at line $LINENO." >&2' ERR

echo "Building $BUILD_TYPE APK..."
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
if ! ./gradlew clean ":app:assemble$BUILD_TYPE" > "$TMP" 2>&1; then
    cat "$TMP"
    echo "ERROR: Gradle $BUILD_TYPE build failed." >&2
    exit 1
fi
if [[ ! -f "$APK" ]]; then
    echo "ERROR: Gradle succeeded but APK not found: $APK" >&2
    exit 1
fi
echo "Build OK: $PWD/$APK"

[[ "$NOINSTALL" == 1 ]] && exit 0

if [[ -n "$DEVICE" ]]; then
    if ! device_state=$(adb -s "$DEVICE" get-state); then
        echo "ERROR: Cannot access Android device: $DEVICE" >&2; exit 1
    fi
    if [[ "$device_state" != device ]]; then
        echo "ERROR: Android device $DEVICE is not ready: $device_state" >&2; exit 1
    fi
    DEVICES=("$DEVICE")
else
    if ! device_list=$(adb devices); then
        echo "ERROR: Cannot list Android devices; see ADB error above." >&2; exit 1
    fi
    mapfile -t DEVICES < <(awk 'NR>1 && $2=="device"{print $1}' <<< "$device_list")
fi

if [ ${#DEVICES[@]} -eq 0 ]; then
    echo "No devices connected" >&2
    exit 1
fi

echo "Devices (${#DEVICES[@]}):"
i=1
for d in "${DEVICES[@]}"; do
    if ! name=$(adb -s "$d" shell getprop ro.product.model); then
        echo "WARN: Cannot read model for device $d; trying market name." >&2; name=""
    fi
    name=${name//$'\r'/}
    if [[ -z "$name" ]]; then
        if ! name=$(adb -s "$d" shell getprop ro.product.marketname); then
            echo "WARN: Cannot read market name for device $d; installing without model name." >&2; name=""
        fi
        name=${name//$'\r'/}
    fi
    echo "  $i) $d  ${name:-unknown model}"
    i=$((i+1))
done

for d in "${DEVICES[@]}"; do
    echo "Installing on $d..."
    if ! adb -s "$d" install -r "$APK" > "$TMP" 2>&1; then
        cat "$TMP" >&2
        echo "ERROR: Cannot install $BUILD_TYPE APK on device $d: $APK" >&2
        install_output=$(cat "$TMP")
        if [[ "$install_output" == *INSTALL_FAILED_UPDATE_INCOMPATIBLE* ]]; then
            echo "ERROR: APK signature differs from the installed app; update requires the same signing key." >&2
            if [[ "$BUILD_TYPE" == Debug ]]; then
                echo "For an installed release, run: ./build.sh --release $d" >&2
            else
                echo "For an installed debug build, run: ./build.sh $d" >&2
                echo "For an installed release, use the original signing/release.p12 and signing/keystore.properties." >&2
            fi
        fi
        exit 1
    fi
    cat "$TMP"
done
echo "Install OK"
