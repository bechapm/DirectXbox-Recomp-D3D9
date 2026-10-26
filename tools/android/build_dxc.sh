#!/bin/sh
# build_dxc.sh - DXC (libdxcompiler.so) for Android arm64, with the NDK.
#
# No one publishes an Android build of DXC, and the Vulkan renderer compiles
# the HLSL the shader generators emit at run time (rhi_vulkan_dxc.cpp), so
# the APK carries one. DXC is LLVM: a cross build needs the host's
# llvm-tblgen and clang-tblgen, built first from the same source.
#
#   tools/android/build_dxc.sh <dxc source> <work dir> [ndk]
#
# The source is a checkout of microsoft/DirectXShaderCompiler with its
# submodules, at the tag the Linux build uses (tools/linux/fetch_dxc.sh):
#
#   git clone --depth 1 --branch v1.9.2609 --recurse-submodules \
#       --shallow-submodules https://github.com/microsoft/DirectXShaderCompiler.git
#
# Leaves <work dir>/android/out/lib/libdxcompiler.so (stripped) and
# <work dir>/android/out/include/dxc/{dxcapi.h,WinAdapter.h}, the layout
# src/d3d/CMakeLists.txt looks for under third_party/dxc.
set -eu

SRC=$(cd "$1" && pwd)
WORK=$2
NDK=${3:-${ANDROID_NDK_HOME:-}}
API=${XBR_ANDROID_API:-30}

if [ -z "$NDK" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "build_dxc.sh: give the NDK as the third argument or ANDROID_NDK_HOME" >&2
    exit 1
fi
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)

# Tests and the SPIR-V tools' own tests are off: they need the full host
# toolchain the cross build does not have, and nothing here runs them.
# HLSL_DISABLE_SOURCE_GENERATION uses the generated headers checked in with
# the source: regenerating them runs the host's clang-format over them and
# fails the build when its version formats differently from DXC's.
COMMON="-G Ninja -C $SRC/cmake/caches/PredefinedParams.cmake
  -DCMAKE_BUILD_TYPE=Release -DHLSL_DISABLE_SOURCE_GENERATION=ON
  -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF
  -DLLVM_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_TESTS=OFF
  -DLLVM_APPEND_VC_REV=OFF"

# 1. The host's table generators.
if [ ! -x "$WORK/host/bin/clang-tblgen" ]; then
    # shellcheck disable=SC2086
    cmake -S "$SRC" -B "$WORK/host" $COMMON
    ninja -C "$WORK/host" llvm-tblgen clang-tblgen
fi

# 2. dxcompiler for arm64-v8a. Exceptions and RTTI are DXC's own choice
# (PredefinedParams); libc++ is linked statically so the .so needs nothing
# from the APK beside it.
# shellcheck disable=SC2086
cmake -S "$SRC" -B "$WORK/android" $COMMON \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
    -DANDROID_STL=c++_static \
    -DLLVM_TABLEGEN="$WORK/host/bin/llvm-tblgen" \
    -DCLANG_TABLEGEN="$WORK/host/bin/clang-tblgen" \
    -DLLVM_HOST_TRIPLE=aarch64-linux-android
ninja -C "$WORK/android" dxcompiler

mkdir -p "$WORK/android/out/lib" "$WORK/android/out/include/dxc"
STRIP=$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -n 1)
"$STRIP" --strip-unneeded -o "$WORK/android/out/lib/libdxcompiler.so" \
    "$WORK/android/lib/libdxcompiler.so"
cp "$SRC/include/dxc/dxcapi.h" "$SRC/include/dxc/WinAdapter.h" \
   "$WORK/android/out/include/dxc/"
ls -l "$WORK/android/out/lib/libdxcompiler.so"
