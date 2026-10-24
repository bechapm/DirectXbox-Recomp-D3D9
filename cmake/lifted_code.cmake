# Lifted code: the flags its sources build with, and the note about GCC.
#
# Included by the toolkit's CMakeLists and by a prebuilt engine package's
# (tools/release/engine_package), so a title's lifted code builds the same
# way whichever it is pointed at.

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
