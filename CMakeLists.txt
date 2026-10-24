cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.9.0 LANGUAGES C CXX)

# A single-config generator (Ninja, Makefiles) with no build type compiles with
# no optimisation at all. The runtime libraries hard-code -O2, but a title's
# executable and its lifted gen/*.c only get the per-config flags, so a plain
# `cmake -S . -B build -G Ninja` built every lifted function at -O0. Default to
# Release here: every title reaches this file through add_subdirectory, and the
# build type is a cache variable, so the title's own targets pick it up too.
# Multi-config generators (Visual Studio, Xcode, Ninja Multi-Config) choose the
# configuration at build time and are left alone. Pass -DCMAKE_BUILD_TYPE=Debug
# (or RelWithDebInfo) to choose otherwise.
get_property(_xboxrecomp_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _xboxrecomp_multi_config AND NOT CMAKE_BUILD_TYPE)
    message(STATUS "No CMAKE_BUILD_TYPE given; defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type for single-config generators (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()
unset(_xboxrecomp_multi_config)

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Libraries:
#   xbox_kernel    - Xbox kernel replacement layer (Xbox kernel ordinals → Win32)
#   xbox_d3d8      - D3D8 → D3D11 graphics compatibility
#   xbox_dsound    - DirectSound → software mixer compatibility
#   xbox_input     - Xbox gamepad → XInput compatibility
#   xbox_apu       - MCPX APU audio emulation (extracted from xemu)
#   xbox_nv2a      - NV2A GPU emulation (extracted from xemu)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions for all targets
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# An implicit declaration is an error, everywhere, in every configuration.
#
# C89's rule is that an undeclared function returns int, and on this target
# that silently truncates a 64-bit return value to 32 bits and sign extends
# it. Seven files used getenv, malloc or calloc without <stdlib.h> and so
# received corrupted pointers: RECOMP_WATCHDOG_SECS faulted on
# 0xFFFFFFFFA94859D5 about one run in three and was inert the rest of the
# time, and two D3D8 draw paths were allocating through a truncated malloc.
#
# It was a warning the whole time (C4013, alongside C4047 for the assignment).
# A warning among hundreds in a build this size is not a signal, so it is an
# error now. C4047 stays a warning: it fires on legitimate casts elsewhere.
if(MSVC)
    add_compile_options(/we4013)
else()
    add_compile_options(-Werror=implicit-function-declaration)
endif()

# Option to build examples

# ── The runtime, built here or imported ───────────────────────
#
# XBOXRECOMP_PREBUILT names a toolkit build made with --target
# xboxrecomp_engine; a title configured with it imports those libraries
# instead of building them (cmake/prebuilt_engine.cmake). Only xbox_config,
# which carries the title's own settings, and the launcher function are taken
# from here as usual.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/prebuilt_engine.cmake)
set(XBOXRECOMP_PREBUILT "" CACHE PATH
    "A toolkit build to take the runtime libraries from (cmake/prebuilt_engine.cmake)")
if(XBOXRECOMP_PREBUILT AND NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(APPLE AND XBOXRECOMP_MACOS_APP)
        message(FATAL_ERROR "XBOXRECOMP_MACOS_APP is not supported with "
                            "XBOXRECOMP_PREBUILT yet: leave one of them off")
    endif()
    xboxrecomp_import_engine("${XBOXRECOMP_PREBUILT}")
    add_subdirectory(src/config)
    add_subdirectory(src/launcher)
else()
    # ── SDL3 ───────────────────────────────────────────────────────
    # The host platform layer: pads (every platform, all controllers, with
    # XInput kept behind RECOMP_PAD_API=xinput on Windows), and on POSIX the
    # window and audio. Built statically from third_party/SDL (release-3.4.16,
    # `git submodule update --init third_party/SDL`) so nothing ships beside
    # the executable; a system SDL3 is the fallback when the submodule is not
    # checked out. Everything below links SDL3::SDL3.
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/third_party/SDL/CMakeLists.txt)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        # Not used by the runtime; leaving them out keeps the build short.
        set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
        set(SDL_GPU OFF CACHE BOOL "" FORCE)
        # Except the renderer on Linux, which is how the launcher shows its
        # frames there (src/launcher/launcher_sdl.c): Wayland gives a window no
        # way to show a buffer but a GPU API, and SDL's window surface falls back
        # to the renderer for it. macOS draws through Core Animation instead.
        if(UNIX AND NOT APPLE)
            set(SDL_RENDER ON CACHE BOOL "" FORCE)
        else()
            set(SDL_RENDER OFF CACHE BOOL "" FORCE)
        endif()
        add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)
        if(NOT TARGET SDL3::SDL3)
            add_library(SDL3::SDL3 ALIAS SDL3-static)
        endif()
    else()
        find_package(SDL3 CONFIG REQUIRED)
    endif()

    # ── miniupnpc ─────────────────────────────────────────────────
    # So a System Link host can ask its router to forward the port friends join
    # on (src/net). Static, from third_party/miniupnp (miniupnpc 2.3.3, BSD-3,
    # `git submodule update --init third_party/miniupnp`). Without it the build
    # still works and the host says it cannot open the router.
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/third_party/miniupnp/miniupnpc/CMakeLists.txt)
        set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
        set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
        add_subdirectory(third_party/miniupnp/miniupnpc EXCLUDE_FROM_ALL)
    endif()

    # ── Runtime Libraries ─────────────────────────────────────────

    # Platform: Win32/NT type vocabulary + POSIX implementation of generic
    # Win32 host primitives. Carries no Xbox semantics; provides the foundation
    # the kernel HLE and the graphics/audio/input layers build on.
    add_subdirectory(src/platform)

    # Player settings, read by the d3d backend, the kernel and the HLE, so it
    # comes before all three and depends on none of them.
    add_subdirectory(src/config)

    # System Link's router and address helpers, for the kernel and the launcher.
    add_subdirectory(src/net)

    # The launcher a title ships beside its game. Defines recomp_add_launcher
    # for a title's CMakeLists to call; builds nothing on its own.
    add_subdirectory(src/launcher)

    # Core: Xbox kernel replacement (memory layout, file I/O, threading, sync)
    add_subdirectory(src/kernel)

    # Graphics: D3D8 → D3D11 compatibility layer
    add_subdirectory(src/d3d)

    # Audio: DirectSound → software mixer compatibility
    add_subdirectory(src/audio)

    # Audio: MCPX APU hardware emulation (voice processor, DSP)
    add_subdirectory(src/apu)

    # GPU: NV2A hardware emulation (register handlers, MMIO, push buffer)
    add_subdirectory(src/nv2a)

    # USB: MCPX OHCI host controllers. A title's XAPI is statically linked and
    # drives these registers directly, so a pad only exists if they do.
    add_subdirectory(src/usb)

    # Input: Xbox gamepad → XInput
    add_subdirectory(src/input)

    # Shared FMV player (Media Foundation -> D3D8 texture)
    add_subdirectory(src/video)

    # The host shell off Windows: the main-thread loop and the game window (SDL3).
    if(NOT WIN32)
        add_subdirectory(src/host)
    endif()

    # XDK functions replaced by name (tools/recomp/hle.py)
    add_subdirectory(src/hle)

    # Frame replay tool: replays a D3D8 capture into the host renderer with no
    # game running (src/hle/d3d8_capture.h). Windowless off Windows.
    add_subdirectory(src/replay)
endif()

# ── Tests ─────────────────────────────────────────────────────

# Offline tests that need no game files; run with ctest. Only when this is the
# top-level project, so a title that pulls the toolkit in with
# add_subdirectory() does not build them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests/nv2a_vsh)
    add_subdirectory(tests/d3d8_capture)        # portable: the capture container
    add_subdirectory(tests/host_timer)          # portable: the sub-millisecond timer
    add_subdirectory(tests/syslink_hub)         # portable: the System Link switch, on 127.0.0.1
    add_subdirectory(tests/net_nat)             # portable: the router behind syslink = nat, on 127.0.0.1
    add_subdirectory(tests/console_id)          # portable: the console identity loader, on a made-up EEPROM
    add_subdirectory(tests/title_update)        # portable: title updates, the launch defaults
    add_subdirectory(tests/mmio_decode)         # x86-64 decoder; skips on other CPUs
    add_subdirectory(tests/kernel_file_rename)  # FileRenameInformation, either file backend
    add_subdirectory(tests/kernel_handle_dup)   # NtDuplicateObject's token, either file backend
    add_subdirectory(tests/audio_dsp)           # portable: speaker fold-down, I3DL2 reverb
    add_subdirectory(tests/input_mouse)         # portable: mouse: bindings and mouse look
    if(NOT WIN32)
        add_subdirectory(tests/host_window)     # needs a display; run by hand
        add_subdirectory(tests/audio_output)    # muted; skips with no device
        add_subdirectory(tests/d3d8_replay)     # the renderer, headless (needs a Vulkan device)
        add_subdirectory(tests/guest_memory)    # POSIX memory backend semantics
        add_subdirectory(tests/mmio_decode_a64) # arm64 fault emulation vs hardware
        add_subdirectory(tests/guest_faults)    # trapped pages via the signal path
        add_subdirectory(tests/memory_layout)   # the real layout, 64 and 128 MB maps
        add_subdirectory(tests/xbe_patches)     # a patched lift loaded from the unpatched XBE
        add_subdirectory(tests/compat_threads)  # POSIX thread and fiber shims
        add_subdirectory(tests/kernel_directory_posix) # NtClose releases a directory search
    endif()
    # Built under src/d3d (tests/d3d8_smoke); pure parsing, no device.
    add_test(NAME d3d8_final_default COMMAND d3d8_final_default)
    # Built under src/config (tests/config_smoke): the settings file, and
    # what one widescreen switch resolves to for each kind of title. It
    # names its own file (RECOMP_DISPLAY_CONFIG) and touches no per-user one.
    add_test(NAME config_smoke COMMAND config_smoke)
    if(WIN32)
        add_subdirectory(tests/nv2a_vsh_hlsl)       # D3DCompile
        add_subdirectory(tests/nv2a_combiners_hlsl)
    else()
        add_test(NAME d3d8_smoke COMMAND d3d8_smoke)    # the format layer
        if(XBOXRECOMP_DXC_INCLUDE)
            add_subdirectory(tests/nv2a_vsh_hlsl)       # DXC to SPIR-V
            add_subdirectory(tests/nv2a_combiners_hlsl)
        endif()
    endif()
endif()

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
add_library(xboxrecomp INTERFACE)
target_link_libraries(xboxrecomp INTERFACE
    xbox_kernel
    xbox_d3d8
    xbox_dsound
    xbox_input
    xbox_usb
    xbox_apu
    xbox_nv2a
    xbox_video
    xbox_hle
)

# The toolkit built on its own is an engine a title can import.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    xboxrecomp_export_engine()
endif()

# ── Lifted code compiles without warnings ─────────────────────
#
# A title's lifted code (src/recomp/gen/*.c) is machine output, tens of MB
# per title, and its warnings are a few patterns repeated a thousand times:
# on clang, `if ((a == b))` gives -Wparentheses-equality 1,022 times in
# TimeSplitters 2, against 59 warnings in everything else the title builds.
# Those 59 are the ones a person can act on, and the generated ones hide
# them. So generated sources build with warnings off, and the rest of a title
# (main.c, recomp_manual.c, the runtime) keeps its warnings.
#
# Implicit declaration stays an error, in generated code as everywhere else
# (see the top of this file): it is how a helper the lifter emits but
# recomp_types.h lacks shows up, and the math.h note in recomp_types.h is
# what discarding it once cost. clang and GCC keep an explicit -Werror= under
# -w; MSVC gets /we4013 after /w for the same reason.
#
# This reaches every title without editing its CMakeLists. A title pulls the
# runtime in with add_subdirectory() before it declares its executable, so
# the work is deferred to the end of the title's own top-level CMakeLists,
# when its targets exist. A source counts as generated when it sits in a
# src/recomp/gen/ directory, which is where tools/recomp writes it.
# -DXBOXRECOMP_GENERATED_WARNINGS=ON shows them again, for lifter work.
#
# Lifted code also gets -fwrapv off MSVC, with warnings on or off. The lifter
# writes x86 arithmetic as C arithmetic on int32_t -- imul is
# `(uint32_t)((int32_t)a * (int32_t)b)` -- and guest code overflows those all
# the time. Signed overflow is undefined in C: MSVC compiles it as the
# wrap-around x86 gives, but GCC and clang may assume it never happens and
# drop or reorder the arithmetic around it. -fwrapv makes it wrap on every
# host, so a title computes on Linux and macOS what it computes on Windows.
# Measured on TimeSplitters 2 with clang, it costs no compile time.
option(XBOXRECOMP_GENERATED_WARNINGS
       "Report compiler warnings in lifted code (src/recomp/gen)" OFF)

# Lifted code takes the build type's optimisation (-O3 in Release) unless
# XBOXRECOMP_LIFTED_OPT names another, off MSVC and outside Debug. -O1 builds
# it faster at a cost in speed that depends on the title. Measured with clang
# on a 6-core machine, lifted code only, against -O3: TimeSplitters 2 26.5 s
# against 32.1 s to compile, but 10% slower uncapped in Siberia (344 against
# 383 fps); Turok: Evolution no slower. -O2 compiles no faster than -O3 here.
# So the default is the full optimisation, and -O1 is there for iteration.
# (It was the default in #84 as XBOXRECOMP_GENERATED_OPT, renamed so build
# folders configured then pick up this default.)
unset(XBOXRECOMP_GENERATED_OPT CACHE)
set(XBOXRECOMP_LIFTED_OPT "" CACHE STRING
    "Optimisation level for lifted code off MSVC: empty for the build type's own, or 0, 1, 2, 3, s")

function(xboxrecomp_generated_source_options)
    if(MSVC)
        set(_opts)
        if(NOT XBOXRECOMP_GENERATED_WARNINGS)
            list(APPEND _opts /w /we4013)
        endif()
    else()
        set(_opts -fwrapv)
        if(NOT XBOXRECOMP_LIFTED_OPT STREQUAL "")
            list(APPEND _opts "$<$<NOT:$<CONFIG:Debug>>:-O${XBOXRECOMP_LIFTED_OPT}>")
        endif()
        if(NOT XBOXRECOMP_GENERATED_WARNINGS)
            list(APPEND _opts -w -Werror=implicit-function-declaration)
        endif()
    endif()
    get_property(_targets DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "EXECUTABLE" AND TARGET xboxrecomp_engine_refresh)
            add_dependencies(${_t} xboxrecomp_engine_refresh)
        endif()
        if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
            continue()
        endif()
        get_target_property(_dir ${_t} SOURCE_DIR)
        get_target_property(_srcs ${_t} SOURCES)
        set(_gen)
        foreach(_s IN LISTS _srcs)
            if(_s MATCHES "\\$<")
                continue()      # a generator expression: not a plain path
            endif()
            cmake_path(ABSOLUTE_PATH _s BASE_DIRECTORY "${_dir}" NORMALIZE
                       OUTPUT_VARIABLE _abs)
            if(_abs MATCHES "/src/recomp/gen/[^/]+\\.c$")
                list(APPEND _gen "${_abs}")
            endif()
        endforeach()
        if(_gen AND _opts)
            set_property(SOURCE ${_gen} DIRECTORY "${_dir}"
                         APPEND PROPERTY COMPILE_OPTIONS ${_opts})
            list(LENGTH _gen _n)
            string(JOIN " " _shown ${_opts})
            string(REGEX REPLACE "\\$<\\$<NOT:\\$<CONFIG:Debug>>:([^>]*)>" "\\1 (not in Debug)"
                   _shown "${_shown}")
            if(XBOXRECOMP_GENERATED_WARNINGS)
                set(_hint "")
            else()
                set(_hint " (XBOXRECOMP_GENERATED_WARNINGS=ON to see their warnings)")
            endif()
            message(STATUS "xboxrecomp: ${_t}: ${_n} lifted sources build with "
                           "${_shown}${_hint}")
        endif()
    endforeach()
endfunction()

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
                   CALL xboxrecomp_generated_source_options)
endif()

# ── Lifted code compiles about three times faster with clang ──
#
# Measured on TimeSplitters 2's 55 lifted files (53 MB of C) on a 6-core
# Linux machine: GCC 15 -O2 90 s, clang 22 -O2 28 s, with smaller code from
# clang. The time is optimisation spread over dozens of passes, not parsing
# or headers, so no flag closes the gap. macOS already builds with clang;
# on Linux the compiler is whatever CC/CXX named at the first configure, and
# scripts/compat_matrix.py (regress.py, the matrix) picks clang when it is on
# PATH. Say so once, at configure time, when a Linux build has GCC instead.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID STREQUAL "GNU"
   AND NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(NOTICE "xboxrecomp: lifted code builds about 3x faster with clang "
                   "than with GCC. For a fresh build directory: CC=clang "
                   "CXX=clang++ (tools/linux/install_deps.sh installs them).")
endif()

