#!/bin/bash
# Собирает отдельный комплект библиотек FFmpeg и кодеков для GUI на Linux или Windows/MSYS2.
# Устанавливает инструменты, загружает исходники, применяет патч и собирает в ffmpeg_build/install; готовый профиль пропускает.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SCRIPT_DIR/../../../proxy.sh" ]; then
    . "$SCRIPT_DIR/../../../proxy.sh" || { echo 'ERROR: cannot load proxy.sh' >&2; exit 1; }
fi
BUILD_DIR="$SCRIPT_DIR/ffmpeg_build"
SRC_DIR="$BUILD_DIR/src"
INSTALL_DIR="$BUILD_DIR/install"
JOBS=$(nproc 2>/dev/null || echo 4)
AMF_HEADERS_VERSION="${AMF_HEADERS_VERSION:-1.5.3}"
FFMPEG_PROFILE_MARKER="$INSTALL_DIR/share/utun-chatgui-ffmpeg-minimal-v8"

case "$(uname -s)" in
    Linux)                      PLATFORM=linux ;;
    MINGW*|MSYS*|CYGWIN*)       PLATFORM=windows ;;
    *) echo "Unsupported platform: $(uname -s)"; exit 1 ;;
esac

log()  { echo "[FFmpeg] $1"; }
die()  { echo "[FFmpeg] ERROR: $1" >&2; exit 1; }
check_cmd() { command -v "$1" >/dev/null 2>&1 || die "$1 not found. Install it first."; }

do_clone() {
    local name="$1" url="$2" tag="$3"
    if [ ! -d "$SRC_DIR/$name" ]; then
        log "Clone $name..."
        for i in 1 2 3; do
            git clone --depth 1 --branch "$tag" "$url" "$SRC_DIR/$name" 2>&1 && break
            log "Clone attempt $i failed, retrying..."
            rm -rf "$SRC_DIR/$name"
            sleep 2
        done
        [ -d "$SRC_DIR/$name/.git" ] || die "Failed to clone $name after 3 attempts"
    else
        log "Skip clone $name: already exists"
    fi
}

# ============================================================================
# Step 0: System dependencies
# ============================================================================
if [ -f "$INSTALL_DIR/lib/pkgconfig/libavcodec.pc" ] && [ -f "$FFMPEG_PROFILE_MARKER" ]; then
    log "FFmpeg already built, skipping package checks and source downloads."
    log "Done: $INSTALL_DIR"
    exit 0
fi

if [ "$PLATFORM" = "linux" ]; then
    MISSING=""
    for pkg in build-essential cmake nasm pkg-config git; do
        dpkg -s "$pkg" >/dev/null 2>&1 || MISSING="$MISSING $pkg"
    done
    if [ -n "$MISSING" ]; then
        log "Installing build tools:$MISSING"
        sudo --preserve-env=http_proxy,https_proxy,HTTP_PROXY,HTTPS_PROXY,no_proxy,NO_PROXY apt update -qq &&
            sudo --preserve-env=http_proxy,https_proxy,HTTP_PROXY,HTTPS_PROXY,no_proxy,NO_PROXY apt install -y $MISSING
    fi
    HW_DEPS="libva-dev libvpl-dev"
    for pkg in $HW_DEPS; do
        if ! dpkg -s "$pkg" >/dev/null 2>&1; then
            log "Installing $pkg (HW accel)"
            sudo --preserve-env=http_proxy,https_proxy,HTTP_PROXY,HTTPS_PROXY,no_proxy,NO_PROXY apt install -y "$pkg"
        fi
    done
else
    check_cmd pacman
    for pkg in mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-nasm \
               mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-libvpl; do
        pacman -Qi "$pkg" >/dev/null 2>&1 || { log "Installing $pkg"; pacman -S --noconfirm "$pkg"; }
    done
fi

check_cmd cmake
check_cmd nasm
check_cmd pkg-config
check_cmd git
check_cmd curl
check_cmd patch

# ============================================================================
# Step 1: Codec libraries (static .a, built with -fPIC)
# ============================================================================
mkdir -p "$SRC_DIR" "$INSTALL_DIR/lib/pkgconfig"

export PKG_CONFIG_PATH="$INSTALL_DIR/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# --- x264 (H.264 encoder) ---
if [ ! -f "$INSTALL_DIR/lib/pkgconfig/x264.pc" ]; then
    do_clone x264 https://code.videolan.org/videolan/x264.git stable
    cd "$SRC_DIR/x264"
    log "Configure x264..."
    ./configure --prefix="$INSTALL_DIR" --enable-static --disable-shared \
        --disable-opencl --disable-avs --disable-swscale --disable-lavf \
        --disable-ffms --disable-gpac --disable-lsmash \
        --extra-cflags="-fPIC" --disable-cli
    log "Build x264..."
    make -j"$JOBS"
    make install
    log "x264 OK"
else
    log "Skip x264: already built"
fi

# --- x265 (H.265/HEVC encoder) ---
if [ ! -f "$INSTALL_DIR/lib/pkgconfig/x265.pc" ]; then
    do_clone x265 https://bitbucket.org/multicoreware/x265_git.git master
    if [ -f "$SRC_DIR/x265/.git/shallow" ]; then
        log "Fetch x265 history and release tags..."
        git -C "$SRC_DIR/x265" fetch --unshallow --tags origin
    fi
    X265_TAG=$(git -C "$SRC_DIR/x265" describe --abbrev=0 --tags 2>/dev/null || true)
    [ -n "$X265_TAG" ] || die "Cannot determine x265 release tag; check the x265 Git remote and tags"
    cd "$SRC_DIR/x265"
    mkdir -p build && cd build
    log "Configure x265..."
    cmake ../source -G "Unix Makefiles" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DENABLE_CLI=OFF -DENABLE_SHARED=OFF \
        -DHIGH_BIT_DEPTH=OFF -DENABLE_HDR10_PLUS=OFF
    log "Build x265..."
    cmake --build . -j"$JOBS"
    cmake --install .
    cat > "$INSTALL_DIR/lib/pkgconfig/x265.pc" <<EOF
prefix=$INSTALL_DIR
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: x265
Description: H.265/HEVC video encoder
Version: 4.1
Libs: -L\${libdir} -lx265
Libs.private: -lstdc++ -lm -lpthread
Cflags: -I\${includedir}
EOF
    log "x265 OK"
else
    log "Skip x265: already built"
fi

# --- libvpx (VP8/VP9 encoder) ---
if [ ! -f "$INSTALL_DIR/lib/pkgconfig/vpx.pc" ]; then
    do_clone libvpx https://chromium.googlesource.com/webm/libvpx main
    cd "$SRC_DIR/libvpx"
    log "Configure libvpx..."
    ./configure --prefix="$INSTALL_DIR" --enable-static --disable-shared \
        --enable-pic --enable-vp8 --enable-vp9 --disable-vp9-highbitdepth \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests
    log "Build libvpx..."
    make -j"$JOBS"
    make install
    log "libvpx OK"
else
    log "Skip libvpx: already built"
fi

# --- libaom (reference AV1 encoder) ---
if [ ! -f "$INSTALL_DIR/lib/pkgconfig/aom.pc" ]; then
    LIBAOM_TAG=v3.13.2
    do_clone libaom https://aomedia.googlesource.com/aom "$LIBAOM_TAG"
    if ! git -C "$SRC_DIR/libaom" describe --tags --exact-match 2>/dev/null | grep -Fxq "$LIBAOM_TAG"; then
        log "Update existing libaom source to $LIBAOM_TAG..."
        git -C "$SRC_DIR/libaom" fetch --depth 1 origin "refs/tags/$LIBAOM_TAG:refs/tags/$LIBAOM_TAG"
        git -C "$SRC_DIR/libaom" checkout --detach "$LIBAOM_TAG"
    fi
    cd "$SRC_DIR/libaom"
    mkdir -p build && cd build
    log "Configure libaom..."
    cmake .. -G "Unix Makefiles" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DBUILD_SHARED_LIBS=OFF -DENABLE_TESTS=OFF \
        -DENABLE_TOOLS=OFF -DENABLE_DOCS=OFF \
        -DENABLE_NASM=ON
    log "Build libaom..."
    cmake --build . -j"$JOBS"
    cmake --install .
    log "libaom OK"
else
    log "Skip libaom: already built"
fi

# --- nv-codec-headers (NVIDIA NVENC/NVDEC headers, no build needed) ---
if [ ! -f "$INSTALL_DIR/include/ffnvcodec/nvEncodeAPI.h" ]; then
    do_clone nv-codec-headers https://github.com/FFmpeg/nv-codec-headers.git master
    cd "$SRC_DIR/nv-codec-headers"
    log "Install nv-codec-headers..."
    make install PREFIX="$INSTALL_DIR"
    log "nv-codec-headers OK"
else
    log "Skip nv-codec-headers: already installed"
fi

# ============================================================================
# Step 2: FFmpeg
# ============================================================================
if [ "$PLATFORM" = "windows" ] && [ ! -f "$INSTALL_DIR/include/AMF/core/Factory.h" ]; then
    AMF_ARCHIVE="$SRC_DIR/AMF-headers-v$AMF_HEADERS_VERSION.tar.gz"
    AMF_EXTRACT_DIR="$SRC_DIR/amf-headers-$AMF_HEADERS_VERSION"
    mkdir -p "$AMF_EXTRACT_DIR" "$INSTALL_DIR/include/AMF"
    if [ ! -f "$AMF_ARCHIVE" ]; then
        log "Download AMD AMF headers v$AMF_HEADERS_VERSION..."
        curl -fL --retry 3 -o "$AMF_ARCHIVE" \
            "https://github.com/GPUOpen-LibrariesAndSDKs/AMF/releases/download/v$AMF_HEADERS_VERSION/AMF-headers-v$AMF_HEADERS_VERSION.tar.gz"
    fi
    log "Extract AMD AMF headers..."
    tar -xzf "$AMF_ARCHIVE" -C "$AMF_EXTRACT_DIR"
    AMF_FACTORY_HEADER=$(find "$AMF_EXTRACT_DIR" -type f -path '*/AMF/core/Factory.h' -print -quit)
    [ -n "$AMF_FACTORY_HEADER" ] || die "AMF headers archive does not contain AMF/core/Factory.h"
    AMF_HEADER_ROOT="${AMF_FACTORY_HEADER%/AMF/core/Factory.h}"
    cp -a "$AMF_HEADER_ROOT/AMF/." "$INSTALL_DIR/include/AMF/"
    log "AMD AMF headers installed in $INSTALL_DIR/include/AMF"
fi

FFMPEG_SRC="$SRC_DIR/ffmpeg"
if [ ! -d "$FFMPEG_SRC" ]; then
    log "Download FFmpeg 7.1.2..."
    FFMPEG_TARBALL="$SRC_DIR/ffmpeg-7.1.2.tar.xz"
    if [ ! -f "$FFMPEG_TARBALL" ]; then
        for url in \
            "https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz" \
            "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n7.1.2.tar.gz"; do
            curl -L --retry 3 -o "$FFMPEG_TARBALL" "$url" 2>&1 && break
            log "Download from $url failed, trying next..."
            rm -f "$FFMPEG_TARBALL"
        done
        [ -f "$FFMPEG_TARBALL" ] || die "Failed to download FFmpeg tarball"
        log "Download OK ($(du -h "$FFMPEG_TARBALL" | cut -f1))"
    fi
    log "Extract FFmpeg..."
    mkdir -p "$FFMPEG_SRC"
    tar -xf "$FFMPEG_TARBALL" -C "$FFMPEG_SRC" --strip-components=1
    rm -f "$FFMPEG_TARBALL"
fi

cd "$SRC_DIR/ffmpeg"
AMF_QP_PATCH="$SCRIPT_DIR/patches/ffmpeg-7.1.2-amf-live-qp.patch"
if ! grep -q 'utun_qp_applied' libavcodec/amfenc.c; then
    log "Apply live AMF QP ceiling support..."
    patch --batch --forward -p1 < "$AMF_QP_PATCH" || die "Failed to apply live AMF QP ceiling patch"
fi
log "Configure FFmpeg..."

CONFIGURE_ARGS=(
    --prefix="$INSTALL_DIR"
    --disable-everything --disable-network
    --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample
    --enable-shared --disable-static
    --enable-gpl --enable-version3
    --pkg-config-flags=--static
    --extra-cflags="-I$INSTALL_DIR/include"
    --extra-ldflags="-L$INSTALL_DIR/lib"
    --enable-libx264 --enable-libx265 --enable-libvpx --enable-libaom --enable-libvpl --enable-nvenc
    --enable-demuxer=avi --enable-demuxer=matroska --enable-demuxer=mov
    --enable-protocol=file --enable-parsers
    # hevc_metadata подключает HEVC CBS parser, необходимый также для trace_headers.
    --enable-bsf=h264_mp4toannexb --enable-bsf=hevc_mp4toannexb --enable-bsf=trace_headers --enable-bsf=hevc_metadata
    --disable-programs --disable-doc --disable-avdevice
    --disable-avfilter --disable-postproc --disable-muxers --enable-muxer=matroska
    --disable-ffprobe --disable-ffplay --disable-ffmpeg --disable-debug --enable-optimizations
)

if [ "$PLATFORM" = "windows" ]; then
    CONFIGURE_ARGS+=(--extra-libs="-lpthread -lm")
else
    CONFIGURE_ARGS+=(--extra-libs="-lpthread -lm -ldl")
fi

DECODERS=(h264 hevc av1 vp8 vp9 mpeg4 mpeg2video mpeg1video mjpeg h263 msmpeg4v3 wmv1 wmv2 vc1
    aac mp3 opus vorbis flac ac3 eac3 alac wmav1 wmav2 pcm_s16le pcm_s24le pcm_s32le pcm_u8 pcm_f32le)
ENCODERS=(mjpeg libx264 libx265 libvpx_vp9 libaom_av1 h264_nvenc hevc_nvenc av1_nvenc
    h264_qsv hevc_qsv vp9_qsv av1_qsv)

for decoder in "${DECODERS[@]}"; do CONFIGURE_ARGS+=("--enable-decoder=$decoder"); done
for encoder in "${ENCODERS[@]}"; do CONFIGURE_ARGS+=("--enable-encoder=$encoder"); done

if [ "$PLATFORM" = "windows" ]; then
    CONFIGURE_ARGS+=(
        --enable-amf --enable-d3d11va
        # FFmpeg 7.1.2 добавляет dxva2_h264/hevc.o только по legacy-флагам; файлы содержат также d3d11va2.
        --enable-hwaccel=h264_d3d11va --enable-hwaccel=hevc_d3d11va
        --enable-hwaccel=h264_d3d11va2 --enable-hwaccel=hevc_d3d11va2
        --enable-encoder=h264_amf --enable-encoder=hevc_amf --enable-encoder=av1_amf
    )
else
    CONFIGURE_ARGS+=(
        --enable-vaapi
        --enable-hwaccel=h264_vaapi --enable-hwaccel=hevc_vaapi
        --enable-encoder=h264_vaapi --enable-encoder=hevc_vaapi
        --enable-encoder=vp9_vaapi --enable-encoder=av1_vaapi
    )
fi

"./configure" "${CONFIGURE_ARGS[@]}"

grep -Eq '^#define CONFIG_CBS_H265 1$' config.h || die "FFmpeg configure did not enable the HEVC header parser"
grep -Eq '^#define CONFIG_TRACE_HEADERS_BSF 1$' config_components.h || die "FFmpeg configure did not enable trace_headers"
grep -Eq '^#define CONFIG_MJPEG_ENCODER 1$' config_components.h || die "FFmpeg configure did not enable MJPEG encoder required for video previews"
log "MJPEG encoder enabled for first-frame JPEG previews"
grep -Eq '^#define CONFIG_MATROSKA_MUXER 1$' config_components.h || die "FFmpeg configure did not enable Matroska recording"
log "Matroska muxer enabled for desktop video recording"
if [ "$PLATFORM" = "windows" ]; then
    grep -Eq '^#define CONFIG_H264_D3D11VA_HWACCEL 1$' config_components.h || die "H.264 D3D11VA implementation object unavailable"
    grep -Eq '^#define CONFIG_HEVC_D3D11VA_HWACCEL 1$' config_components.h || die "HEVC D3D11VA implementation object unavailable"
    grep -Eq '^#define CONFIG_H264_D3D11VA2_HWACCEL 1$' config_components.h || die "H.264 D3D11VA device decoder unavailable"
    grep -Eq '^#define CONFIG_HEVC_D3D11VA2_HWACCEL 1$' config_components.h || die "HEVC D3D11VA device decoder unavailable"
else
    grep -Eq '^#define CONFIG_H264_VAAPI_HWACCEL 1$' config_components.h || die "H.264 VAAPI decoder unavailable"
    grep -Eq '^#define CONFIG_HEVC_VAAPI_HWACCEL 1$' config_components.h || die "HEVC VAAPI decoder unavailable"
fi

if [ "$PLATFORM" = "windows" ]; then
    for encoder in CONFIG_H264_AMF_ENCODER CONFIG_HEVC_AMF_ENCODER; do
        grep -Eq "^#define $encoder 1$" config_components.h \
            || die "FFmpeg configure did not enable $encoder; check AMF headers and config.log"
    done
fi

log "Build FFmpeg..."
make clean
make -j"$JOBS"
make install

mkdir -p "$(dirname "$FFMPEG_PROFILE_MARKER")"
touch "$FFMPEG_PROFILE_MARKER"

log "==============================================================="
log "FFmpeg build SUCCESS"
log "Install prefix: $INSTALL_DIR"
log "Libraries: $(ls "$INSTALL_DIR/lib/"*.so* "$INSTALL_DIR/lib/"*.dll.a "$INSTALL_DIR/bin/"*.dll 2>/dev/null | tr '\n' ' ')"
log "==============================================================="
