#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-linux"
FFMPEG_PREFIX="${IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX:-}"
QT_PREFIX="${QT_PREFIX:-}"
OUTPUT_DIR="${ROOT_DIR}/package"
VERSION="0.1.0"
ARCH="$(uname -m)"
IAMF_TOOLS_REVISION="${IAMF_TOOLS_GIT_TAG:-5c5da5436324ea7882db64314845a4d9049f211d}"
FFMPEG_REVISION="${FFMPEG_GIT_TAG:-}"
FFMPEG_LICENSE_FILE=""
NO_BUILD=0

usage() {
    cat <<'EOF'
Usage: package_linux.sh [options]
  --build-dir DIR
  --qt-prefix DIR
  --ffmpeg-prefix DIR
  --output-dir DIR
  --version VERSION
  --arch ARCH
  --no-build
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --qt-prefix) QT_PREFIX="$2"; shift 2 ;;
        --ffmpeg-prefix) FFMPEG_PREFIX="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --arch) ARCH="$2"; shift 2 ;;
        --no-build) NO_BUILD=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$NO_BUILD" -eq 0 ]]; then
    cmake --build "$BUILD_DIR" --config Release
fi

STAGE_DIR="${OUTPUT_DIR}/linux-stage"
PACKAGE_NAME="iamf_muxer-${VERSION}-linux-${ARCH}"
PACKAGE_DIR="${OUTPUT_DIR}/${PACKAGE_NAME}"
TAR_PATH="${OUTPUT_DIR}/${PACKAGE_NAME}.tar.gz"
APPIMAGE_PATH="${OUTPUT_DIR}/${PACKAGE_NAME}.AppImage"
rm -rf "$STAGE_DIR" "$PACKAGE_DIR" "$TAR_PATH" "$APPIMAGE_PATH"
mkdir -p "$OUTPUT_DIR"
cmake --install "$BUILD_DIR" --prefix "$STAGE_DIR" --config Release

if [[ -z "$FFMPEG_PREFIX" || ! -d "${FFMPEG_PREFIX}/lib" ]]; then
    echo "--ffmpeg-prefix must point to an installed FFmpeg prefix" >&2
    exit 1
fi
find "${STAGE_DIR}/lib" -maxdepth 1 -type f \
    -name 'libiamf_muxer_*.a' -delete 2>/dev/null || true

GUI_PATH="${STAGE_DIR}/bin/iamf_muxer"
if [[ ! -x "$GUI_PATH" ]]; then
    echo "Installed Linux GUI executable was not found: $GUI_PATH" >&2
    exit 1
fi
if [[ -n "$FFMPEG_PREFIX" && -d "${FFMPEG_PREFIX}/lib" ]]; then
    mkdir -p "${STAGE_DIR}/lib"
    find "${FFMPEG_PREFIX}/lib" -maxdepth 1 -type f \
        \( -name 'libavformat*.so*' -o -name 'libavcodec*.so*' -o -name 'libavutil*.so*' -o -name 'libswresample*.so*' \) \
        -exec cp -L {} "${STAGE_DIR}/lib/" \;
fi
if [[ -z "$(find "${STAGE_DIR}/lib" -maxdepth 1 -type f -name 'libavformat*.so*' -print -quit)" ]]; then
    echo "The staged Linux package is missing the FFmpeg avformat runtime" >&2
    exit 1
fi
if [[ -z "$FFMPEG_REVISION" && -f "${FFMPEG_PREFIX}/FFmpeg-revision.txt" ]]; then
    FFMPEG_REVISION="$(tr -d '\r\n' < "${FFMPEG_PREFIX}/FFmpeg-revision.txt")"
fi
FFMPEG_REVISION="${FFMPEG_REVISION:-unknown}"
if [[ -f "${FFMPEG_PREFIX}/FFmpeg-LICENSES.txt" ]]; then
    FFMPEG_LICENSE_FILE="${FFMPEG_PREFIX}/FFmpeg-LICENSES.txt"
fi
if command -v patchelf >/dev/null 2>&1; then
    patchelf --set-rpath '$ORIGIN/../lib' "$GUI_PATH"
fi

if ! command -v linuxdeploy >/dev/null 2>&1 ||
   ! command -v linuxdeploy-plugin-qt >/dev/null 2>&1; then
    if [[ -z "$QT_PREFIX" || ! -d "${QT_PREFIX}/lib" ]]; then
        echo "QtPrefix is required when linuxdeploy is unavailable" >&2
        exit 1
    fi
    mkdir -p "${STAGE_DIR}/lib" "${STAGE_DIR}/plugins" "${STAGE_DIR}/qml"
    find "${QT_PREFIX}/lib" -maxdepth 1 -type f -name 'lib*.so*' \
        -exec cp -L {} "${STAGE_DIR}/lib/" \;
    if [[ -d "${QT_PREFIX}/plugins" ]]; then
        cp -R "${QT_PREFIX}/plugins/." "${STAGE_DIR}/plugins/"
    fi
    if [[ -d "${QT_PREFIX}/qml" ]]; then
        cp -R "${QT_PREFIX}/qml/." "${STAGE_DIR}/qml/"
    fi
    cat > "${STAGE_DIR}/bin/qt.conf" <<'EOF'
[Paths]
Prefix=..
Libraries=lib
Plugins=plugins
Qml2Imports=qml
EOF
fi

IAMF_SOURCE="${BUILD_DIR}/_deps/official_iamf_tools-src"
METADATA_ARGS=(
    "-DSTAGE_DIR=${STAGE_DIR}"
    "-DPROJECT_ROOT=${ROOT_DIR}"
    "-DVERSION=${VERSION}"
    "-DPLATFORM=linux"
    "-DARCH=${ARCH}"
    "-DIAMF_TOOLS_REVISION=${IAMF_TOOLS_REVISION}"
    "-DFFMPEG_REVISION=${FFMPEG_REVISION}"
    "-DSPDLOG_VERSION=1.17.0"
    "-DSPDLOG_FMT_VERSION=12.1.0"
    -P "${ROOT_DIR}/cmake/write_release_metadata.cmake"
)
if [[ -n "$FFMPEG_LICENSE_FILE" ]]; then
    METADATA_ARGS=("-DFFMPEG_LICENSE_FILE=${FFMPEG_LICENSE_FILE}" "${METADATA_ARGS[@]}")
fi
if [[ -f "${IAMF_SOURCE}/LICENSE" ]]; then
    METADATA_ARGS=("-DIAMF_TOOLS_SOURCE_DIR=${IAMF_SOURCE}" "${METADATA_ARGS[@]}")
fi
if [[ -f "${BUILD_DIR}/_deps/spdlog-src/LICENSE" ]]; then
    METADATA_ARGS=("-DSPDLOG_LICENSE_FILE=${BUILD_DIR}/_deps/spdlog-src/LICENSE" "${METADATA_ARGS[@]}")
fi
if [[ -f "${BUILD_DIR}/_deps/spdlog-src/include/spdlog/fmt/bundled/fmt.license.rst" ]]; then
    METADATA_ARGS=("-DSPDLOG_FMT_LICENSE_FILE=${BUILD_DIR}/_deps/spdlog-src/include/spdlog/fmt/bundled/fmt.license.rst" "${METADATA_ARGS[@]}")
fi
cmake "${METADATA_ARGS[@]}"

mkdir -p "$PACKAGE_DIR"
cp -R "$STAGE_DIR/." "$PACKAGE_DIR/"
tar -C "$OUTPUT_DIR" -czf "$TAR_PATH" "$PACKAGE_NAME"

if command -v linuxdeploy >/dev/null 2>&1 && command -v linuxdeploy-plugin-qt >/dev/null 2>&1; then
    APPDIR="${OUTPUT_DIR}/linux-appdir"
    rm -rf "$APPDIR"
    mkdir -p "$APPDIR/usr"
    cp -R "$STAGE_DIR/." "$APPDIR/usr/"
    export QMAKE="${QT_PREFIX}/bin/qmake"
    linuxdeploy --appdir "$APPDIR" --plugin qt --output appimage \
        --custom-apprun "$ROOT_DIR/scripts/linux_apprun.sh"
    if [[ -f "${OUTPUT_DIR}/iamf_muxer-x86_64.AppImage" ]]; then
        mv "${OUTPUT_DIR}/iamf_muxer-x86_64.AppImage" "$APPIMAGE_PATH"
    fi
fi

echo "Linux package: $TAR_PATH"
if [[ -f "$APPIMAGE_PATH" ]]; then
    echo "Linux AppImage: $APPIMAGE_PATH"
fi
