#!/usr/bin/env bash
# fetch_dxc.sh - put DXC (the HLSL compiler the Vulkan renderer loads at run
# time) into third_party/dxc, from Microsoft's Linux release tarball.
#
#   bash tools/linux/fetch_dxc.sh [release-tag]
#
# The tarball is a Unix prefix (include/dxc, lib/libdxcompiler.so, bin/dxc),
# which src/d3d/CMakeLists.txt looks for as third_party/dxc/include/dxc; the
# build then records lib/libdxcompiler.so as the compiler's last fallback, so a
# build run from this tree needs nothing installed. Distributions do not
# package DXC, and the Vulkan SDK for Linux is a 1 GB download for this one
# library. No sudo; nothing outside third_party/dxc is touched.
set -euo pipefail

TAG="${1:-v1.9.2609}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="$ROOT/third_party/dxc"
API="https://api.github.com/repos/microsoft/DirectXShaderCompiler/releases/tags/$TAG"

case "$(uname -m)" in
    x86_64) ;;
    *) echo "fetch_dxc.sh: Microsoft ships DXC for x86-64 Linux only; build it from source on $(uname -m)" >&2
       exit 1 ;;
esac

url=$(curl -fsSL "$API" | grep -o '"browser_download_url": *"[^"]*linux_dxc[^"]*\.tar\.gz"' |
      sed 's/.*"\(https[^"]*\)"/\1/' | head -1)
if [ -z "$url" ]; then
    echo "fetch_dxc.sh: no Linux tarball in DXC release $TAG" >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "Fetching $url"
curl -fsSL -o "$tmp/dxc.tar.gz" "$url"
mkdir "$tmp/x"
tar -xzf "$tmp/dxc.tar.gz" -C "$tmp/x"
if [ ! -f "$tmp/x/include/dxc/dxcapi.h" ] || [ ! -f "$tmp/x/lib/libdxcompiler.so" ]; then
    echo "fetch_dxc.sh: the tarball is not laid out as expected (include/dxc, lib/)" >&2
    exit 1
fi

rm -rf "$DEST/include" "$DEST/lib" "$DEST/bin"
mkdir -p "$DEST"
cp -r "$tmp/x/include" "$tmp/x/lib" "$tmp/x/bin" "$DEST/"
cp "$tmp/x"/LICENSE-*.txt "$DEST/" 2>/dev/null || true
echo "DXC $TAG in $DEST (reconfigure CMake to pick it up)"
