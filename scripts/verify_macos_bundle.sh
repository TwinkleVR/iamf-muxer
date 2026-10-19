#!/usr/bin/env bash
set -euo pipefail

APP_PATH="${1:?Usage: verify_macos_bundle.sh APP_PATH [ARCH] [MIN_MACOS_VERSION]}"
EXPECTED_ARCH="${2:-arm64}"
MAX_MIN_MACOS="${3:-14.0}"

if [[ ! -d "$APP_PATH" ]]; then
    echo "Application bundle was not found: $APP_PATH" >&2
    exit 1
fi
for tool in file lipo otool awk grep; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Required verification tool was not found: $tool" >&2
        exit 1
    fi
done

compare_versions() {
    awk -v actual="$1" -v maximum="$2" 'BEGIN {
        split(actual, a, ".")
        split(maximum, b, ".")
        for (i = 1; i <= 3; i++) {
            av = a[i] + 0
            bv = b[i] + 0
            if (av < bv) exit 0
            if (av > bv) exit 1
        }
        exit 0
    }'
}

checked=0
while IFS= read -r -d '' binary; do
    description="$(file -b "$binary")"
    [[ "$description" == *"Mach-O"* ]] || continue

    architectures="$(lipo -archs "$binary" 2>/dev/null || true)"
    if [[ " $architectures " != *" ${EXPECTED_ARCH} "* ]]; then
        echo "Missing ${EXPECTED_ARCH} architecture: $binary ($architectures)" >&2
        exit 1
    fi

    min_versions="$(otool -l "$binary" | awk '
        /cmd LC_BUILD_VERSION/ { mode = "build"; next }
        /cmd LC_VERSION_MIN_MACOSX/ { mode = "legacy"; next }
        mode == "build" && $1 == "minos" { print $2; mode = "" }
        mode == "legacy" && $1 == "version" { print $2; mode = "" }
    ')"
    if [[ -z "$min_versions" ]]; then
        echo "Could not read a macOS deployment target: $binary" >&2
        exit 1
    fi
    while IFS= read -r min_version; do
        if ! compare_versions "$min_version" "$MAX_MIN_MACOS"; then
            echo "${binary} requires macOS ${min_version}, above ${MAX_MIN_MACOS}" >&2
            exit 1
        fi
    done <<< "$min_versions"

    if otool -L "$binary" | grep -Eq '^[[:space:]]+(/opt/homebrew/|/usr/local/Cellar/)'; then
        echo "Unbundled Homebrew dependency remains: $binary" >&2
        otool -L "$binary" >&2
        exit 1
    fi
    checked=$((checked + 1))
done < <(find "$APP_PATH" -type f -print0)

if [[ "$checked" -eq 0 ]]; then
    echo "No Mach-O files were found in: $APP_PATH" >&2
    exit 1
fi
printf 'Verified %s Mach-O files: %s architecture present, minimum macOS <= %s\n' \
    "$checked" "$EXPECTED_ARCH" "$MAX_MIN_MACOS"
