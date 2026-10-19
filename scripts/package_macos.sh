#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-macos"
QT_PREFIX="${QT_PREFIX:-}"
FFMPEG_PREFIX="${IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX:-}"
OUTPUT_DIR="${ROOT_DIR}/package"
VERSION="$(tr -d '\r\n' < "${ROOT_DIR}/VERSION.txt")"
ARCH="$(uname -m)"
MIN_MACOS_VERSION="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
IAMF_TOOLS_REVISION="${IAMF_TOOLS_GIT_TAG:-5c5da5436324ea7882db64314845a4d9049f211d}"
FFMPEG_REVISION="${FFMPEG_GIT_TAG:-}"
FFMPEG_LICENSE_FILE=""
NO_BUILD=0

usage() {
    cat <<'EOF'
Usage: package_macos.sh [options]
  --build-dir DIR
  --qt-prefix DIR
  --ffmpeg-prefix DIR
  --output-dir DIR
  --version VERSION
  --arch ARCH
  --min-macos-version VERSION
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
        --min-macos-version) MIN_MACOS_VERSION="$2"; shift 2 ;;
        --no-build) NO_BUILD=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$NO_BUILD" -eq 0 ]]; then
    cmake --build "$BUILD_DIR" --config Release
fi

STAGE_DIR="${OUTPUT_DIR}/macos-stage"
PACKAGE_NAME="iamf-muxer-${VERSION}-macos-${ARCH}"
PACKAGE_DIR="${OUTPUT_DIR}/${PACKAGE_NAME}"
ZIP_PATH="${OUTPUT_DIR}/${PACKAGE_NAME}.zip"
rm -rf "$STAGE_DIR" "$PACKAGE_DIR" "$ZIP_PATH"
mkdir -p "$OUTPUT_DIR"
cmake --install "$BUILD_DIR" --prefix "$STAGE_DIR" --config Release

if [[ -z "$FFMPEG_PREFIX" || ! -d "${FFMPEG_PREFIX}/lib" ]]; then
    echo "--ffmpeg-prefix must point to an installed FFmpeg prefix" >&2
    exit 1
fi

APP_PATH="$(find "$STAGE_DIR" -maxdepth 2 -type d -name 'iamf-muxer.app' -print -quit)"
if [[ -z "$APP_PATH" ]]; then
    echo "Installed macOS app bundle was not found below $STAGE_DIR" >&2
    exit 1
fi
if [[ -z "$QT_PREFIX" ]]; then
    echo "--qt-prefix is required for macdeployqt" >&2
    exit 1
fi
MACDEPLOYQT="${QT_PREFIX}/bin/macdeployqt"
if [[ ! -x "$MACDEPLOYQT" ]]; then
    echo "macdeployqt was not found: $MACDEPLOYQT" >&2
    exit 1
fi

FRAMEWORKS_DIR="${APP_PATH}/Contents/Frameworks"
mkdir -p "$FRAMEWORKS_DIR"
if [[ -d "${STAGE_DIR}/bin" ]]; then
    find "${STAGE_DIR}/bin" -maxdepth 1 -type f \
        \( -name 'libiamf_tools_encoder_inprocess*.dylib' -o -name 'iamf_tools_encoder_inprocess*.dylib' \) \
        -exec cp -L {} "$FRAMEWORKS_DIR/" \;
fi
if [[ -n "$FFMPEG_PREFIX" && -d "${FFMPEG_PREFIX}/lib" ]]; then
    find "${FFMPEG_PREFIX}/lib" -maxdepth 1 \( -type f -o -type l \) \
        \( -name 'libavformat*.dylib' -o -name 'libavcodec*.dylib' -o -name 'libavutil*.dylib' -o -name 'libswresample*.dylib' \) \
        -exec cp -L {} "$FRAMEWORKS_DIR/" \;
fi
if [[ -z "$(find "$FRAMEWORKS_DIR" -maxdepth 1 -type f -name 'libavformat*.dylib' -print -quit)" ]]; then
    echo "The macOS app bundle is missing the FFmpeg avformat runtime" >&2
    exit 1
fi
if [[ -z "$FFMPEG_REVISION" && -f "${FFMPEG_PREFIX}/FFmpeg-revision.txt" ]]; then
    FFMPEG_REVISION="$(tr -d '\r\n' < "${FFMPEG_PREFIX}/FFmpeg-revision.txt")"
fi
FFMPEG_REVISION="${FFMPEG_REVISION:-unknown}"
if [[ -f "${FFMPEG_PREFIX}/FFmpeg-LICENSES.txt" ]]; then
    FFMPEG_LICENSE_FILE="${FFMPEG_PREFIX}/FFmpeg-LICENSES.txt"
fi

"$MACDEPLOYQT" "$APP_PATH" "-qmldir=${ROOT_DIR}/qml" \
    "-libpath=${FRAMEWORKS_DIR}" -no-plugins -always-overwrite
# Ship runtime Qt plugins while omitting optional SQL drivers that may depend
# on machine-local ODBC/PostgreSQL/Mimer libraries. This app doesn't use Qt SQL.
PLUGINS_DIR="${APP_PATH}/Contents/PlugIns"
for plugin_category in iconengines imageformats multimedia networkinformation platforms quick tls; do
    SOURCE_PLUGIN_DIR="${QT_PREFIX}/plugins/${plugin_category}"
    if [[ -d "$SOURCE_PLUGIN_DIR" ]]; then
        DEST_PLUGIN_DIR="${PLUGINS_DIR}/${plugin_category}"
        mkdir -p "$DEST_PLUGIN_DIR"
        if [[ "$plugin_category" == "multimedia" ]]; then
            find "$SOURCE_PLUGIN_DIR" -maxdepth 1 -type f -name '*.dylib' \
                ! -name 'libffmpegmediaplugin.dylib' \
                -exec cp -L {} "$DEST_PLUGIN_DIR/" \;
        else
            find "$SOURCE_PLUGIN_DIR" -maxdepth 1 -type f -name '*.dylib' \
                -exec cp -L {} "$DEST_PLUGIN_DIR/" \;
        fi
    fi
done
while IFS= read -r -d '' plugin; do
    if ! otool -l "$plugin" | grep -Fq '@loader_path/../../Frameworks'; then
        install_name_tool -add_rpath '@loader_path/../../Frameworks' "$plugin" 2>/dev/null
    fi
done < <(find "$PLUGINS_DIR" -type f -name '*.dylib' -print0)
APP_EXECUTABLE="${APP_PATH}/Contents/MacOS/iamf-muxer"
if command -v install_name_tool >/dev/null 2>&1 && [[ -x "$APP_EXECUTABLE" ]]; then
    install_name_tool -add_rpath '@executable_path/../Frameworks' "$APP_EXECUTABLE" 2>/dev/null || true
fi
codesign --force --deep --sign - --timestamp=none "$APP_PATH"

"${ROOT_DIR}/scripts/verify_macos_bundle.sh" \
    "$APP_PATH" "$ARCH" "$MIN_MACOS_VERSION"

IAMF_SOURCE="${BUILD_DIR}/_deps/official_iamf_tools-src"
METADATA_ARGS=(
    "-DSTAGE_DIR=${STAGE_DIR}"
    "-DPROJECT_ROOT=${ROOT_DIR}"
    "-DVERSION=${VERSION}"
    "-DPLATFORM=macos"
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
cp -R "$APP_PATH" "$PACKAGE_DIR/"
for item in VERSION.txt THIRD_PARTY_NOTICES.txt licenses; do
    if [[ -e "${STAGE_DIR}/${item}" ]]; then
        cp -R "${STAGE_DIR}/${item}" "$PACKAGE_DIR/"
    fi
done
ditto -c -k --sequesterRsrc --keepParent "$PACKAGE_DIR" "$ZIP_PATH"

echo "macOS package: $ZIP_PATH"
