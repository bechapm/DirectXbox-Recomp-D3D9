# xbox_host -- the host shell off Windows (host.h): the main-thread event
# loop src/platform's host_main_posix runs, and the game window the D3D8
# replacement draws into. Windows keeps its Win32 window (src/hle/hle_d3d8.c)
# and does not build this.

add_library(xbox_host STATIC
    host_sdl.c
    host_mac.c
)
target_include_directories(xbox_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# rhi.h, for RhiNativeWindow only: the handles the renderer presents to.
target_include_directories(xbox_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../d3d)
target_link_libraries(xbox_host PUBLIC SDL3::SDL3)
# recomp_window_fit: the window's size against the screen, the same rule
# as the Win32 window in src/hle. xbox_config depends on nothing.
target_link_libraries(xbox_host PRIVATE xbox_config)
find_package(Threads REQUIRED)
target_link_libraries(xbox_host PUBLIC Threads::Threads)
if(APPLE)
    target_link_libraries(xbox_host PUBLIC "-framework AppKit" "-framework Foundation" objc)
endif()
target_compile_options(xbox_host PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ── A macOS application bundle for the title (scripts/make_macos_app.py) ──
#
#   cmake -DXBOXRECOMP_MACOS_APP=ON [-DXBOXRECOMP_APP_NAME="TimeSplitters 2"] ...
#
# adds a target <title>_app, built with the title, that wraps the executable in
# <name>.app beside it: the Vulkan loader, MoltenVK and its manifest, DXC and
# every non-system library it links, an Info.plist, an ad-hoc signature. The
# game folder goes beside the .app (or in Contents/Resources/game).
#
# FOR YOUR OWN MACHINE ONLY: the .app holds the title's recompiled code. Never
# add one to CI artifacts or releases (CLAUDE.md, "Legal").
#
# No title CMakeLists has to change: the target is added at the end of the
# top-level (title) directory, for the title's own executable, which is named
# after its project.
if(APPLE)
    option(XBOXRECOMP_MACOS_APP "Also build <title>.app (macOS; for this machine only)" OFF)
    option(XBOXRECOMP_MACOS_APP_DRY_RUN "The <title>_app target only prints what it would do" OFF)
    set(XBOXRECOMP_APP_NAME "" CACHE STRING "The .app's name (default: the title's executable name)")
    if(XBOXRECOMP_MACOS_APP)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        # The whole command is built here, at configure time, in this
        # directory, and handed to the deferred call as a finished list in a
        # cache variable. The deferred call runs in the title's directory,
        # where this directory's variables do not exist: once, the "bundle"
        # command came out as the bare executable, and building the target ran
        # the game -- unmuted, in front, on the player's saves. So: every part
        # is checked before the list exists, the list must start with the
        # Python interpreter and the script, and the script itself refuses to
        # start any program but otool, install_name_tool, codesign and cp.
        get_filename_component(xbr_script ${CMAKE_CURRENT_LIST_DIR}/../../scripts/make_macos_app.py ABSOLUTE)
        get_filename_component(xbr_python_name "${Python3_EXECUTABLE}" NAME)
        if(NOT EXISTS "${Python3_EXECUTABLE}" OR NOT xbr_python_name MATCHES "^python" OR
           NOT EXISTS "${xbr_script}")
            message(FATAL_ERROR "XBOXRECOMP_MACOS_APP: no usable Python ('${Python3_EXECUTABLE}') "
                                "or no ${xbr_script}")
        endif()
        set(xbr_cmd "${Python3_EXECUTABLE}" "${xbr_script}")
        if(XBOXRECOMP_MACOS_APP_DRY_RUN)
            list(APPEND xbr_cmd --dry-run)
        endif()
        set(XBR_APP_COMMAND "${xbr_cmd}" CACHE INTERNAL "make_macos_app.py, without its arguments")
        function(xbr_add_title_app)
            set(cmd ${XBR_APP_COMMAND})
            list(LENGTH cmd n)
            list(GET cmd 0 python)
            list(GET cmd 1 script)
            get_filename_component(python_name "${python}" NAME)
            if(n LESS 2 OR NOT python_name MATCHES "^python[0-9.]*$" OR NOT EXISTS "${python}" OR
               NOT script MATCHES "make_macos_app\\.py$")
                message(FATAL_ERROR "XBOXRECOMP_MACOS_APP: bundling command is not the script: '${cmd}'")
            endif()
            set(exe ${CMAKE_PROJECT_NAME})
            if(NOT TARGET ${exe})
                message(WARNING "XBOXRECOMP_MACOS_APP: no executable target '${exe}' to bundle")
                return()
            endif()
            set(name "${XBOXRECOMP_APP_NAME}")
            if(name STREQUAL "")
                set(name ${exe})
            endif()
            add_custom_target(${exe}_app ALL
                COMMAND ${cmd} $<TARGET_FILE:${exe}> --name "${name}" --out $<TARGET_FILE_DIR:${exe}>
                DEPENDS ${exe}
                COMMENT "Bundling ${name}.app (for this machine only)"
                VERBATIM)
        endfunction()
        cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL xbr_add_title_app)
    endif()
endif()
