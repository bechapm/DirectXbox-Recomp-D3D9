#!/bin/bash
# The LunarG Vulkan SDK for macOS, pinned and checked, installed without a
# window or an administrator: what a Mac self-building release takes the
# Vulkan loader, MoltenVK and DXC from, and the engine's DXC headers
# (tools/release/make_release.py). For CI; a desk has the SDK installed.
#
#   tools/macos/fetch_vulkan_sdk.sh <install dir> [<download cache>]
#
# Prints VULKAN_SDK=<install dir>/macOS, for $GITHUB_ENV.
set -euo pipefail

VERSION=1.4.357.1
SHA256=cf23e604e6b8b82c18eaba329f5623ea5a309d949a98d49a61a161d576c769a5
ROOT="${1:?usage: fetch_vulkan_sdk.sh <install dir> [<download cache>]}"
CACHE="${2:-$ROOT.downloads}"
ZIP="$CACHE/vulkansdk-macos-$VERSION.zip"

if [ -f "$ROOT/macOS/lib/libMoltenVK.dylib" ]; then
    echo "VULKAN_SDK=$ROOT/macOS"
    exit 0
fi
mkdir -p "$CACHE"
if [ ! -f "$ZIP" ] || [ "$(shasum -a 256 "$ZIP" | cut -d' ' -f1)" != "$SHA256" ]; then
    echo "downloading Vulkan SDK $VERSION" >&2
    curl -sSfL -o "$ZIP.part" \
        "https://sdk.lunarg.com/sdk/download/$VERSION/mac/vulkansdk-macos-$VERSION.zip"
    got="$(shasum -a 256 "$ZIP.part" | cut -d' ' -f1)"
    if [ "$got" != "$SHA256" ]; then
        echo "fetch_vulkan_sdk: SHA-256 $got, expected $SHA256" >&2
        rm -f "$ZIP.part"
        exit 1
    fi
    mv "$ZIP.part" "$ZIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
unzip -q "$ZIP" -d "$TMP"
# The core component only: the system-wide one (/usr/local) needs sudo.
"$TMP/vulkansdk-macOS-$VERSION.app/Contents/MacOS/vulkansdk-macOS-$VERSION" \
    --root "$ROOT" --accept-licenses --default-answer --confirm-command \
    install com.lunarg.vulkan.core >&2
test -f "$ROOT/macOS/lib/libMoltenVK.dylib"
echo "VULKAN_SDK=$ROOT/macOS"
