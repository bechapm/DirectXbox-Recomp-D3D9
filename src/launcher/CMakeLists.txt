# The launcher a recompiled title ships beside its game.
#
# Not a library and not built here: each title makes its own, because the
# executable it starts is the title's. A title's CMakeLists calls
#
#     recomp_add_launcher(timesplitters2_launcher timesplitters2_recomp.exe)
#
# or, the same on every host, with the game's own target:
#
#     recomp_add_launcher(gladius_launcher $<TARGET_FILE_NAME:gladius_recomp>)
#
# (add NO_WIDESCREEN for a title where widescreen must not be offered; see
# below. What the one Widescreen switch does for a title that does offer it
# is said with recomp_title_widescreen, src/config/CMakeLists.txt)
#
# and gets the launcher beside the game, reading the game's own XBE for the
# name it shows and the settings file it writes.
#
# Windows, macOS and Linux. launcher.c is the launcher and is the same on
# all three; the window is launcher_win32.c, or launcher_sdl.c (SDL3)
# putting its frames up through launcher_cocoa.m on macOS and an SDL
# renderer on Linux; the text is GDI (launcher_theme_gdi.c), CoreText
# (launcher_theme_coretext.c) or FreeType with fontconfig
# (launcher_theme_freetype.c). Elsewhere, or on a Linux without FreeType's
# or fontconfig's headers, the call is accepted and makes nothing, so a
# title's CMakeLists need not ask which host it is on.

function(recomp_add_launcher TARGET GAME_EXE)
    cmake_parse_arguments(_launcher "NO_WIDESCREEN" "" "" ${ARGN})
    set(_dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})

    if(WIN32)
        add_executable(${TARGET} WIN32
            ${_dir}/launcher.c
            ${_dir}/launcher_win32.c
            ${_dir}/launcher_theme.c
            ${_dir}/launcher_theme_gdi.c
            ${_dir}/launcher_bindings.c
        )
        if(COMMAND xboxrecomp_icon_rc)
            xboxrecomp_icon_rc(_icon_rc)        # cmake/title_icon.cmake
            target_sources(${TARGET} PRIVATE "${_icon_rc}")
        endif()
    elseif(APPLE)
        add_executable(${TARGET}
            ${_dir}/launcher.c
            ${_dir}/launcher_sdl.c
            ${_dir}/launcher_cocoa.m
            ${_dir}/launcher_theme.c
            ${_dir}/launcher_theme_coretext.c
            ${_dir}/launcher_bindings.c
        )
        # Objective-C, compiled by the C compiler (clang on any Mac) so a
        # title need not enable another language for one small file.
        set_source_files_properties(${_dir}/launcher_cocoa.m PROPERTIES
            LANGUAGE C COMPILE_OPTIONS "-xobjective-c;-fobjc-arc")
    elseif(UNIX)
        find_package(Freetype QUIET)
        find_package(Fontconfig QUIET)
        if(NOT Freetype_FOUND OR NOT Fontconfig_FOUND)
            message(STATUS "recomp_add_launcher(${TARGET}): no launcher: it needs "
                           "FreeType and fontconfig with their headers")
            return()
        endif()
        add_executable(${TARGET}
            ${_dir}/launcher.c
            ${_dir}/launcher_sdl.c
            ${_dir}/launcher_theme.c
            ${_dir}/launcher_theme_freetype.c
            ${_dir}/launcher_bindings.c
        )
        target_link_libraries(${TARGET} PRIVATE Freetype::Freetype Fontconfig::Fontconfig m)
    else()
        message(STATUS "recomp_add_launcher(${TARGET}): no launcher on this host")
        return()
    endif()

    # A title written when this was Windows only names its game <name>.exe;
    # off Windows that executable is <name>.
    if(NOT WIN32)
        string(REGEX REPLACE "\\.exe$" "" GAME_EXE "${GAME_EXE}")
    endif()

    target_include_directories(${TARGET} PRIVATE ${_dir})
    target_compile_definitions(${TARGET} PRIVATE
        LAUNCHER_GAME_EXE="${GAME_EXE}"
    )
    # The launcher has to find the game's XBE where the game does, or it
    # names the wrong settings file and every choice made in it is lost.
    # The game looks beside itself in game/, then in YOUR_GAME_DIR from
    # its main.c, for YOUR_GAME_XBE_NAME; read those defines rather than
    # repeat them here, so the two cannot drift. They are C string
    # literals, so separators are doubled backslashes: made forward
    # slashes, which Windows accepts.
    set(_main_c "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")
    if(EXISTS "${_main_c}")
        file(STRINGS "${_main_c}" _dir_line
             REGEX "^#define[ \t]+YOUR_GAME_DIR[ \t]+\"")
        if(_dir_line MATCHES "\"([^\"]*)\"")
            string(REPLACE "\\\\" "/" _game_dir "${CMAKE_MATCH_1}")
            target_compile_definitions(${TARGET} PRIVATE
                LAUNCHER_GAME_DIR="${_game_dir}")
        endif()
        # The image the game boots: default.xbe unless main.c names one, as
        # a disc whose default.xbe only picks a language does. That one has
        # the title's id but not its name.
        file(STRINGS "${_main_c}" _xbe_line
             REGEX "^#define[ \t]+YOUR_GAME_XBE_NAME[ \t]+\"")
        if(_xbe_line MATCHES "\"([^\"]*)\"")
            target_compile_definitions(${TARGET} PRIVATE
                LAUNCHER_GAME_XBE="${CMAKE_MATCH_1}")
        endif()
        # Re-configure when main.c changes, e.g. after regen_title_main.py.
        set_property(DIRECTORY APPEND PROPERTY
            CMAKE_CONFIGURE_DEPENDS "${_main_c}")
    endif()

    # xbox_config carries the settings file's format, so the launcher
    # and the runtime cannot disagree about what they are writing and
    # reading. xbox_input is the pads (recomp_pad.h: SDL3, or XInput),
    # the same code the game reads them with, and brings platform
    # (host_main.h: where this executable is). xbox_net finds the
    # addresses the System Link tab tells a host to give out.
    target_link_libraries(${TARGET} PRIVATE xbox_config xbox_input xbox_net)
    if(WIN32)
        # The rest is in every Windows.
        target_link_libraries(${TARGET} PRIVATE user32 gdi32)
    elseif(APPLE)
        target_link_libraries(${TARGET} PRIVATE SDL3::SDL3
            "-framework CoreText" "-framework CoreGraphics"
            "-framework CoreFoundation" "-framework Cocoa"
            "-framework QuartzCore")
    else()
        target_link_libraries(${TARGET} PRIVATE SDL3::SDL3)
    endif()

    # recomp_add_launcher(<target> <exe> NO_WIDESCREEN): a title where
    # widescreen cannot be right hides the Widescreen row, and the
    # launcher writes it off. A 2D fighter is the case: its stage
    # bounds and walls are built for 4:3, so a wider view shows space the
    # fighters cannot reach.
    if(_launcher_NO_WIDESCREEN)
        target_compile_definitions(${TARGET} PRIVATE LAUNCHER_NO_WIDESCREEN=1)
    endif()
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE
            /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
    else()
        target_compile_options(${TARGET} PRIVATE
            -Wall -Wextra -Wno-unused-parameter $<$<NOT:$<CONFIG:Debug>>:-O2>)
    endif()
endfunction()
