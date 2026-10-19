#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd -P)"
REPOSITORY="${FFMPEG_REPOSITORY:-https://github.com/FFmpeg/FFmpeg.git}"
SOURCE_DIR=""
PREFIX=""
# FFmpeg n9.0.2 release commit (peeled from the annotated tag).
REVISION="${FFMPEG_GIT_TAG:-946fcce07b6dcd0331c8cc609192aeff5e1924f8}"
JOBS=""

FFMPEG_DEMUXERS=(
    aac ac3 aiff amr ape asf avi caf dts dtshd eac3 flac flv
    h264 hevc ivf matroska mov mp3 mpc mpc8 mpegps mpegts mpegvideo mxf
    ogg tta wav w64 wv m4v
)
FFMPEG_DECODERS=(
    aac aac_fixed aac_latm ac3 ac3_fixed alac amrnb amrwb ape dca flac
    gsm gsm_ms mlp mp1 mp1float mp2 mp2float mp3 mp3float mpc7 mpc8 opus
    speex tak truehd tta vorbis wavpack wmalossless wmapro wmav1 wmav2
    wmavoice adpcm_g722 adpcm_g726 adpcm_g726le adpcm_ima_qt adpcm_ima_wav
    adpcm_ms adpcm_yamaha pcm_alaw pcm_bluray pcm_dvd pcm_f16le pcm_f24le
    pcm_f32be pcm_f32le pcm_f64be pcm_f64le pcm_mulaw pcm_s8 pcm_s8_planar
    pcm_s16be pcm_s16be_planar pcm_s16le pcm_s16le_planar pcm_s24be
    pcm_s24daud pcm_s24le pcm_s24le_planar pcm_s32be pcm_s32le
    pcm_s32le_planar pcm_s64be pcm_s64le pcm_u8 pcm_u16be pcm_u16le
    pcm_u24be pcm_u24le pcm_u32be pcm_u32le movtext
)
FFMPEG_PARSERS=(
    aac aac_latm ac3 dca flac gsm mlp mpegaudio opus tak vorbis
    av1 h264 hevc mpeg4video mpegvideo vp8 vp9
)

join_components() {
    local IFS=,
    printf '%s' "$*"
}

if [[ "$(uname -s)" == "Darwin" ]]; then
    # FFmpeg inherits this value through Clang when compiling the dylibs.
    export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
fi

usage() {
    cat <<'EOF'
Usage: build_ffmpeg.sh --source-dir DIR --prefix DIR [options]
  --repository URL
  --source-dir DIR
  --prefix DIR
  --revision COMMIT_OR_TAG
  --jobs N
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repository) REPOSITORY="$2"; shift 2 ;;
        --source-dir) SOURCE_DIR="$2"; shift 2 ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --revision) REVISION="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$SOURCE_DIR" || -z "$PREFIX" ]]; then
    echo "--source-dir and --prefix are required" >&2
    usage >&2
    exit 2
fi
if [[ -z "$JOBS" ]]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then
        JOBS="$(sysctl -n hw.ncpu)"
    else
        JOBS=2
    fi
fi
# macOS Bash 3.2 with nounset rejects expansion of an empty array.
X86ASM_OPTION=""
if ! command -v nasm >/dev/null 2>&1; then
    # NASM is optional for stream-copy muxing and is commonly absent on Windows.
    X86ASM_OPTION="--disable-x86asm"
fi

mkdir -p "$(dirname "$SOURCE_DIR")" "$PREFIX"
SOURCE_DIR="$(cd "$(dirname "$SOURCE_DIR")" && pwd -P)/$(basename "$SOURCE_DIR")"
PREFIX="$(cd "$PREFIX" && pwd -P)"
if [[ ! -d "${SOURCE_DIR}/.git" ]]; then
    git clone --filter=blob:none "$REPOSITORY" "$SOURCE_DIR"
fi
git -C "$SOURCE_DIR" fetch --depth 1 origin "$REVISION"
git -C "$SOURCE_DIR" checkout --detach "$REVISION"
RESOLVED_REVISION="$(git -C "$SOURCE_DIR" rev-parse HEAD)"

if [[ -f "${SOURCE_DIR}/Makefile" ]]; then
    make -C "$SOURCE_DIR" distclean >/dev/null 2>&1 || true
fi
TIMECODE_PATCH="${REPOSITORY_ROOT}/cmake/patches/ffmpeg-mp4-tmcd.patch"
if git -C "$SOURCE_DIR" apply --check "$TIMECODE_PATCH"; then
    git -C "$SOURCE_DIR" apply "$TIMECODE_PATCH"
elif ! git -C "$SOURCE_DIR" apply --reverse --check "$TIMECODE_PATCH"; then
    echo "The FFmpeg source does not match the required MP4 timecode patch" >&2
    exit 1
fi

pushd "$SOURCE_DIR" >/dev/null
DARWIN_INSTALL_NAME_ARGS=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    DARWIN_INSTALL_NAME_ARGS+=(--install-name-dir=@rpath)
fi
# FFmpeg defaults to LGPL with GPL and nonfree components disabled; do not add
# --enable-gpl or --enable-nonfree here.
./configure \
    --prefix="$PREFIX" \
    "${DARWIN_INSTALL_NAME_ARGS[@]}" \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --disable-static \
    --enable-shared \
    --disable-network \
    --disable-autodetect \
    ${X86ASM_OPTION:+"$X86ASM_OPTION"} \
    --disable-everything \
    --enable-avcodec \
    --enable-avformat \
    --enable-avutil \
    --enable-swresample \
    --enable-protocol=file \
    --enable-demuxer="$(join_components "${FFMPEG_DEMUXERS[@]}")" \
    --enable-muxer=mp4 \
    --enable-decoder="$(join_components "${FFMPEG_DECODERS[@]}")" \
    --enable-parser="$(join_components "${FFMPEG_PARSERS[@]}")" \
    --enable-bsf=text2movsub \
    --enable-pic
make -j"$JOBS"
make install
popd >/dev/null

printf '%s\n' "$RESOLVED_REVISION" > "${PREFIX}/FFmpeg-revision.txt"
{
    for license_file in LICENSE.md COPYING.GPLv2 COPYING.GPLv3 COPYING.LGPLv2.1 COPYING.LGPLv3; do
        if [[ -f "${SOURCE_DIR}/${license_file}" ]]; then
            printf '\n===== %s =====\n\n' "$license_file"
            cat "${SOURCE_DIR}/${license_file}"
        fi
    done
} > "${PREFIX}/FFmpeg-LICENSES.txt"
echo "Built FFmpeg revision ${RESOLVED_REVISION} into ${PREFIX}"
