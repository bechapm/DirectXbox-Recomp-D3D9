#!/usr/bin/env bash
# install_deps.sh - Install build dependencies for the native Linux build of
# the xboxrecomp runtime libraries.
#
# Detects the host family from /etc/os-release and uses its package manager:
#   Debian/Ubuntu (incl. WSL) -> apt
#   Arch/CachyOS/Manjaro      -> pacman
#   Fedora/RHEL               -> dnf
#   SteamOS                   -> nothing: how to build in the Freedesktop SDK
#   the Freedesktop SDK       -> nothing: it has everything
#
#   bash tools/linux/install_deps.sh
#
# Requires sudo (will prompt for a password).
set -euo pipefail

# Overridable so the self-check below can feed it fake os-release files.
OS_RELEASE="${OS_RELEASE:-/etc/os-release}"

# Same dependencies throughout, spelled per distro:
#   build toolchain (C and C++) + cmake + ninja + pkg-config + git + curl
#   clang + lld - lifted code compiles about 3x faster with clang than GCC
#           (scripts/compat_matrix.py picks it when it is on PATH; by hand,
#           CC=clang CXX=clang++ at a build directory's first configure)
#   X11 and Wayland headers, PulseAudio and ALSA headers, libudev:
#           SDL3 (third_party/SDL, built static) will not configure without
#           a video backend, and opens its audio and pads through these
#   FreeType + fontconfig - the launcher's text (src/launcher)
#   Python 3 with venv - the pipeline (capstone goes in a venv)
# Not here: the Vulkan loader and DXC, which the renderer loads at run time
# (the loader comes with every desktop's Mesa; DXC is
# tools/linux/fetch_dxc.sh, since no distribution packages it).
#
# SteamOS is told to build in the Freedesktop SDK instead: its system is
# read-only and has no compiler, and pacman there would be undone by the
# next update.
detect_family() {
    local id id_like
    [ -r "$OS_RELEASE" ] || return 1
    id=$(. "$OS_RELEASE" 2>/dev/null && echo "${ID:-}")
    id_like=$(. "$OS_RELEASE" 2>/dev/null && echo "${ID_LIKE:-}")

    case " $id " in
        *" steamos "*) echo steamos; return 0 ;;
        *" org.freedesktop.platform "*) echo flatpak-sdk; return 0 ;;
    esac
    case " $id $id_like " in
        *" debian "*|*" ubuntu "*) echo debian ;;
        *" arch "*|*" archlinux "*) echo arch ;;
        *" fedora "*|*" rhel "*|*" centos "*) echo fedora ;;
        *) return 1 ;;
    esac
}

family=$(detect_family) || {
    echo "Could not identify the distro from $OS_RELEASE." >&2
    echo "Install these by hand, with headers: a C and C++ toolchain, cmake, ninja," >&2
    echo "pkg-config, X11 and Wayland, PulseAudio, ALSA, libudev, FreeType," >&2
    echo "fontconfig, and Python 3 with venv." >&2
    exit 1
}

case "$family" in
    steamos)
        cat <<'MSG'
SteamOS: nothing to install here. Its system is read-only and has no
compiler; build in the Freedesktop SDK, which has every dependency:

  flatpak install flathub org.freedesktop.Sdk//25.08     # if not there yet
  flatpak install flathub org.freedesktop.Sdk.Extension.llvm22//25.08
  flatpak run --filesystem=home --share=network --device=dri \
      --socket=wayland --socket=fallback-x11 --socket=pulseaudio \
      --command=bash org.freedesktop.Sdk//25.08
  source /usr/lib/sdk/llvm22/enable.sh      # inside it: clang on PATH

The LLVM extension is optional but worth it: lifted code compiles about
three times faster with clang than with the SDK's GCC. What it builds also
runs outside it, straight from SteamOS.
MSG
        exit 0
        ;;
    flatpak-sdk)
        echo "The Freedesktop SDK already has every build dependency; nothing to install."
        if ! command -v clang >/dev/null 2>&1; then
            echo
            echo "It has no clang, which builds lifted code about 3x faster than GCC."
            echo "On the host: flatpak install flathub org.freedesktop.Sdk.Extension.llvm22//25.08"
            echo "then, in here: source /usr/lib/sdk/llvm22/enable.sh"
        fi
        exit 0
        ;;
    debian)
        PKGS=(build-essential clang lld cmake ninja-build pkg-config git curl python3-venv
              libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev
              libxss-dev libxfixes-dev libxtst-dev libxkbcommon-dev
              libwayland-dev wayland-protocols libegl-dev libdrm-dev libgbm-dev
              libpulse-dev libasound2-dev libudev-dev
              libfreetype-dev libfontconfig-dev)
        echo "Detected Debian/Ubuntu. Installing ${#PKGS[@]} packages..."
        sudo apt-get update
        sudo apt-get install -y "${PKGS[@]}"
        ;;
    arch)
        # base-devel covers gcc/make/pkgconf; the X11 and Wayland libraries
        # ship their headers.
        PKGS=(base-devel clang lld cmake ninja git curl python
              libx11 libxext libxrandr libxcursor libxi libxss libxfixes libxtst
              libxkbcommon wayland wayland-protocols libdecor mesa libdrm
              libpulse alsa-lib systemd-libs
              freetype2 fontconfig)
        echo "Detected Arch. Installing ${#PKGS[@]} packages..."
        sudo pacman -S --needed "${PKGS[@]}"
        ;;
    fedora)
        PKGS=(gcc gcc-c++ clang lld make cmake ninja-build pkgconf-pkg-config git curl python3
              libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel
              libXScrnSaver-devel libXfixes-devel libXtst-devel libxkbcommon-devel
              wayland-devel wayland-protocols-devel mesa-libEGL-devel libdrm-devel
              mesa-libgbm-devel pulseaudio-libs-devel alsa-lib-devel systemd-devel
              freetype-devel fontconfig-devel)
        echo "Detected Fedora/RHEL. Installing ${#PKGS[@]} packages..."
        sudo dnf install -y "${PKGS[@]}"
        ;;
esac

echo
echo "Done. Build deps installed. Next: bash tools/linux/fetch_dxc.sh"
