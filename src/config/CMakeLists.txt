# Player settings: the file the launcher writes and the runtime reads.
#
# Deliberately depends on nothing. The d3d backend, the kernel and the HLE
# all read settings, so anything this linked would be linked by all three.

add_library(xbox_config STATIC
    recomp_config.c
)

target_include_directories(xbox_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_config PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(xbox_config PRIVATE -Wall -Wextra -O2)
endif()

# What widescreen means for the title being built, said once in its
# CMakeLists, after the add_subdirectory that brings the toolkit in:
#
#     recomp_title_widescreen(NATIVE)                  # has a 16:9 mode
#     recomp_title_widescreen(HOR_PLUS REGISTER 60)    # has none
#     recomp_title_widescreen(NATIVE REGISTER 176 CENTRE_2D)
#     recomp_title_widescreen(HOR_PLUS REGISTER 96 187) # two kinds of program
#
# CENTRE_2D: the game's 2D is drawn for 4:3 even in its 16:9 mode, so keep
# all of it at 4:3 but whole-screen flat-colour passes (fades, letterbox
# bars) -- the widescreen_2d = centre policy, as what widescreen_2d = auto
# means for this title.
#
# The player has one widescreen switch. Whether that is the console flag
# alone or the flag and a wider camera is a fact about the game, and so is
# the vertex constant register its projection is in (REGISTER, optional for
# either mode; without it the register is 60). A title whose programs keep
# their projection in more than one place names each, first the main one:
# Turok: Evolution's rigid programs read a model-view-projection at c96 and
# its skinned ones a view-projection at c187, with their bone palette at
# c96. Each program is widened through the register it reads, and only it. See recomp_config.h,
# "widescreen", for how it is resolved and what a title that says nothing
# gets. It is compiled into this library because the game and its launcher
# both link it, so the two cannot be told different things; a title that
# would rather say it in code calls recomp_widescreen_title.
#
# A title where widescreen cannot be right at all says so to its launcher
# instead: recomp_add_launcher(... NO_WIDESCREEN).
function(recomp_title_widescreen MODE)
    cmake_parse_arguments(_wide "CENTRE_2D" "" "REGISTER" ${ARGN})
    if(MODE STREQUAL "NATIVE")
        set(_mode 1)                                # RECOMP_WIDE_NATIVE
    elseif(MODE STREQUAL "HOR_PLUS")
        set(_mode 2)                                # RECOMP_WIDE_HOR_PLUS
    else()
        message(FATAL_ERROR
            "recomp_title_widescreen: \"${MODE}\" is neither NATIVE nor HOR_PLUS")
    endif()
    if(_wide_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "recomp_title_widescreen: unexpected \"${_wide_UNPARSED_ARGUMENTS}\"")
    endif()
    target_compile_definitions(xbox_config PRIVATE RECOMP_TITLE_WIDESCREEN=${_mode})
    if(_wide_CENTRE_2D)
        target_compile_definitions(xbox_config PRIVATE RECOMP_TITLE_CENTRE_2D=1)
    endif()
    if(DEFINED _wide_REGISTER)
        list(LENGTH _wide_REGISTER _count)
        if(_count GREATER 4)
            message(FATAL_ERROR
                "recomp_title_widescreen: at most 4 REGISTERs, not ${_count}")
        endif()
        foreach(_reg IN LISTS _wide_REGISTER)
            if(NOT _reg MATCHES "^[0-9]+$" OR _reg GREATER 191)
                message(FATAL_ERROR
                    "recomp_title_widescreen: REGISTER ${_reg} is not 0..191")
            endif()
        endforeach()
        list(POP_FRONT _wide_REGISTER _first)
        target_compile_definitions(xbox_config PRIVATE
            RECOMP_TITLE_PROJECTION_REGISTER=${_first})
        if(_wide_REGISTER)
            list(JOIN _wide_REGISTER "," _extra)
            target_compile_definitions(xbox_config PRIVATE
                RECOMP_TITLE_EXTRA_PROJECTION_REGISTERS=${_extra})
        endif()
    endif()
endfunction()

# How the title's logic keeps time, said once in its CMakeLists:
#
#     recomp_title_timing(DELTA)    # steps by a measured clock delta
#     recomp_title_timing(FRAME)    # steps once a frame or a vblank (the default)
#
# DELTA is a promise that the game runs at the right speed at any frame
# rate. Check it before making it: run the same scripted input capped and
# uncapped with RECOMP_HLE_D3D8_DUMP_EVERY_MS and compare the frames at the
# same moments (docs/technical/resolution-and-framerate.md). A DELTA title's
# launcher and F10 offer frame caps above 60 and uncapped; adaptive 60
# stays the default. Turok: Evolution is one.
function(recomp_title_timing KIND)
    if(KIND STREQUAL "DELTA")
        target_compile_definitions(xbox_config PRIVATE RECOMP_TITLE_KEEPS_TIME=1)
    elseif(NOT KIND STREQUAL "FRAME")
        message(FATAL_ERROR
            "recomp_title_timing: \"${KIND}\" is neither DELTA nor FRAME")
    endif()
    if(ARGN)
        message(FATAL_ERROR "recomp_title_timing: unexpected \"${ARGN}\"")
    endif()
endfunction()

# The title's own controls, said once in its CMakeLists:
#
#     recomp_title_input(DEFAULTS config/input_defaults.json MOUSE_LOOK)
#
# DEFAULTS is a binding file in the format input_bindings.json has
# (docs/technical/input-binding.md) holding the layout that suits this
# game: which key is its fire, which its reload. It replaces the toolkit's
# generic layout as what a control reads when nobody has bound it, and what
# the launcher's reset gives back. A title that has one keeps its player's
# bindings in a file of its own (titles/<TITLEID>.input.json in the
# per-user folder) rather than the file every other game shares, because
# its keys mean something only in it.
#
# MOUSE_LOOK makes mouse look on by default for this title; a settings
# file that already says otherwise keeps what it says.
#
# Compiled into this library, like widescreen, because the game and its
# launcher both link it and so cannot be told different things.
function(recomp_title_input)
    cmake_parse_arguments(_in "MOUSE_LOOK" "DEFAULTS" "" ${ARGN})
    if(_in_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "recomp_title_input: unexpected \"${_in_UNPARSED_ARGUMENTS}\"")
    endif()
    if(DEFINED _in_DEFAULTS)
        get_filename_component(_file "${_in_DEFAULTS}" ABSOLUTE
                               BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_file}")
            message(FATAL_ERROR "recomp_title_input: ${_file} does not exist")
        endif()
        set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND PROPERTY
                     CMAKE_CONFIGURE_DEPENDS "${_file}")
        file(READ "${_file}" _json)
        # One C string literal per line of the file.
        string(REPLACE "\\" "\\\\" _json "${_json}")
        string(REPLACE "\"" "\\\"" _json "${_json}")
        string(REPLACE "\r" "" _json "${_json}")
        string(REPLACE "\n" "\\n\"\n    \"" _json "${_json}")
        set(_out "${CMAKE_BINARY_DIR}/recomp_title_input.c")
        file(WRITE "${_out}.tmp"
             "/* Written by recomp_title_input from ${_file}. */\n"
             "const char recomp_title_input_json[] =\n    \"${_json}\";\n")
        configure_file("${_out}.tmp" "${_out}" COPYONLY)    # rewritten only when it changes
        target_sources(xbox_config PRIVATE "${_out}")
        target_compile_definitions(xbox_config PRIVATE RECOMP_TITLE_INPUT_DEFAULTS=1)
    endif()
    if(_in_MOUSE_LOOK)
        target_compile_definitions(xbox_config PRIVATE RECOMP_TITLE_MOUSE_LOOK=1)
    endif()
endfunction()

# Absent from a prebuilt engine package, which carries no tests.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../../tests/config_smoke)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/config_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/config_smoke)
endif()
