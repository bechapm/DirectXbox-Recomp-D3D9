# A prebuilt engine: the runtime libraries built once and shared by titles.
#
# A title's build compiles two things: its own lifted code, and the whole
# runtime (about 450 objects, the kernel, renderer, audio, SDL). The runtime
# is the same for every title, so build it once:
#
#     cmake -S <toolkit> -B <engine> -G Ninja -DCMAKE_BUILD_TYPE=Release \
#           -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#     cmake --build <engine> --target xboxrecomp_engine
#
# and point a title at it, at its first configure:
#
#     cmake -S titles/<name> -B ... -DXBOXRECOMP_PREBUILT=<engine>
#
# The title then imports the libraries under their usual names instead of
# building them, and nothing in its CMakeLists changes. The engine builds in
# about 9 s, once; a clean TimeSplitters 2 build, with lifted code at -O1 as
# well, went from 41 s to 27 s on a 6-core machine.
#
# Two things are not shared. xbox_config carries the title's own widescreen
# and timing settings (recomp_title_widescreen, recomp_title_timing), so it
# is built per title from src/config, and every engine library's link to it
# is pointed at that one: a generic copy is never on the link line to win by
# link order. And the engine must have been built the way the title would
# have built it: the same compiler, build type and RECOMP_ABI_CHECK, which
# the engine records and the import checks, and from this same toolkit tree
# -- an engine from another checkout is refused, so regress.py's two
# worktrees can never share one. Every executable a title builds depends on
# bringing the engine up to date first (a no-op when it is), so a toolkit
# edit reaches the title without anyone remembering to rebuild the engine.

# The libraries a title links, xbox_config included so the export is whole.
function(_xboxrecomp_engine_libs out)
    set(_libs platform xbox_config xbox_net xbox_kernel xbox_d3d8 xbox_dsound
              xbox_samplerate xbox_apu xbox_nv2a xbox_usb xbox_input
              xbox_video xbox_hle)
    foreach(_t xbox_host libminiupnpc-static)
        if(TARGET ${_t})
            list(APPEND _libs ${_t})
        endif()
    endforeach()
    set(${out} ${_libs} PARENT_SCOPE)
endfunction()

# What the import compares against the title's own configure.
function(_xboxrecomp_engine_settings out)
    if(RECOMP_ABI_CHECK)
        set(_abi ON)
    else()
        set(_abi OFF)
    endif()
    set(${out}
        "compiler ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} (${CMAKE_C_COMPILER}) | "
        "c++ ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} | "
        "build type ${CMAKE_BUILD_TYPE} | RECOMP_ABI_CHECK ${_abi}"
        PARENT_SCOPE)
endfunction()

# The targets built here that `libs` link, directly or through others:
# CMake exports a library only with everything it links that is not
# imported. SDL and miniupnpc keep helpers of their own.
function(_xboxrecomp_link_closure out)
    set(_seen)
    set(_todo ${ARGN})
    while(_todo)
        list(POP_FRONT _todo _t)
        get_target_property(_real ${_t} ALIASED_TARGET)
        if(_real)
            set(_t ${_real})
        endif()
        if(_t IN_LIST _seen)
            continue()
        endif()
        get_target_property(_imported ${_t} IMPORTED)
        get_target_property(_dir ${_t} SOURCE_DIR)
        if(_imported OR _dir MATCHES "/third_party/SDL(/|$)")
            continue()      # found again by the title: SDL exports itself
        endif()
        list(APPEND _seen ${_t})
        foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(_deps ${_t} ${_prop})
            if(NOT _deps)
                continue()
            endif()
            foreach(_d IN LISTS _deps)
                # $<LINK_ONLY:x>, $<BUILD_INTERFACE:x> and the like: the name inside.
                string(REGEX REPLACE "^\\$<[A-Z_]+:(.*)>$" "\\1" _d "${_d}")
                if(TARGET "${_d}")
                    list(APPEND _todo "${_d}")
                endif()
            endforeach()
        endforeach()
    endwhile()
    set(${out} ${_seen} PARENT_SCOPE)
endfunction()

# In the toolkit's own build: write the export and the settings beside it.
function(xboxrecomp_export_engine)
    _xboxrecomp_engine_libs(_libs)
    _xboxrecomp_link_closure(_libs ${_libs})
    export(TARGETS ${_libs} NAMESPACE xrengine::
           FILE "${CMAKE_BINARY_DIR}/xboxrecomp-engine-targets.cmake")
    # The names the export gives them (a target may set EXPORT_NAME: miniupnpc's
    # library exports as `miniupnpc`).
    set(_names)
    foreach(_t ${_libs})
        get_target_property(_n ${_t} EXPORT_NAME)
        if(NOT _n)
            set(_n ${_t})
        endif()
        list(APPEND _names ${_n})
    endforeach()
    _xboxrecomp_engine_settings(_settings)
    string(JOIN "" _settings ${_settings})
    # SDL from the submodule leaves a package of its own in the build tree.
    set(_sdl "")
    if(TARGET SDL3-static AND EXISTS "${CMAKE_BINARY_DIR}/third_party/SDL")
        set(_sdl "${CMAKE_BINARY_DIR}/third_party/SDL")
    endif()
    file(WRITE "${CMAKE_BINARY_DIR}/xboxrecomp-engine.cmake"
         "set(XBOXRECOMP_ENGINE_SETTINGS \"${_settings}\")\n"
         "set(XBOXRECOMP_ENGINE_LIBS \"${_names}\")\n"
         "set(XBOXRECOMP_ENGINE_SDL3_DIR \"${_sdl}\")\n"
         "set(XBOXRECOMP_ENGINE_SOURCE_DIR \"${CMAKE_SOURCE_DIR}\")\n")
    add_custom_target(xboxrecomp_engine DEPENDS ${_libs})
endfunction()

# In a title's build: import the engine in place of building it.
function(xboxrecomp_import_engine dir)
    set(_info "${dir}/xboxrecomp-engine.cmake")
    if(NOT EXISTS "${_info}")
        message(FATAL_ERROR "XBOXRECOMP_PREBUILT=${dir} is not an engine build: "
                            "no xboxrecomp-engine.cmake there. Build one with "
                            "--target xboxrecomp_engine (cmake/prebuilt_engine.cmake)")
    endif()
    include("${_info}")
    get_filename_component(_here "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." REALPATH)
    get_filename_component(_there "${XBOXRECOMP_ENGINE_SOURCE_DIR}" REALPATH)
    if(NOT _here STREQUAL _there)
        message(FATAL_ERROR "XBOXRECOMP_PREBUILT=${dir} was built from ${_there}, "
                            "not from this toolkit (${_here}). An engine is only "
                            "valid for the sources it was built from.")
    endif()
    _xboxrecomp_engine_settings(_mine)
    string(JOIN "" _mine ${_mine})
    if(NOT _mine STREQUAL XBOXRECOMP_ENGINE_SETTINGS)
        message(FATAL_ERROR "XBOXRECOMP_PREBUILT=${dir} was built differently:\n"
                            "  engine: ${XBOXRECOMP_ENGINE_SETTINGS}\n"
                            "  title:  ${_mine}\n"
                            "Build the engine to match, or leave XBOXRECOMP_PREBUILT "
                            "unset to build the runtime with the title.")
    endif()
    foreach(_t ${XBOXRECOMP_ENGINE_LIBS})
        if(TARGET xrengine::${_t})
            message(FATAL_ERROR "xboxrecomp: the engine is imported twice")
        endif()
    endforeach()

    # What the libraries link that they do not carry. GLOBAL, because the
    # title's executable, one directory up, links them.
    if(XBOXRECOMP_ENGINE_SDL3_DIR)
        find_package(SDL3 CONFIG REQUIRED GLOBAL
                     PATHS "${XBOXRECOMP_ENGINE_SDL3_DIR}" NO_DEFAULT_PATH)
    else()
        find_package(SDL3 CONFIG REQUIRED GLOBAL)
    endif()
    if(NOT WIN32)
        find_package(Threads REQUIRED GLOBAL)
        find_package(OpenSSL REQUIRED GLOBAL)
    endif()
    include("${dir}/xboxrecomp-engine-targets.cmake")

    foreach(_t ${XBOXRECOMP_ENGINE_LIBS})
        set_property(TARGET xrengine::${_t} PROPERTY IMPORTED_GLOBAL TRUE)
        get_target_property(_links xrengine::${_t} INTERFACE_LINK_LIBRARIES)
        if(_links)
            string(REPLACE "xrengine::xbox_config" "xbox_config" _links "${_links}")
            set_property(TARGET xrengine::${_t} PROPERTY INTERFACE_LINK_LIBRARIES "${_links}")
        endif()
        # Our own libraries keep the names titles and the launcher use.
        if(_t MATCHES "^(xbox_.*|platform)$" AND NOT _t STREQUAL "xbox_config")
            add_library(${_t} ALIAS xrengine::${_t})
        endif()
    endforeach()
    # Brought up to date before anything links it: the title's executables
    # depend on this (xboxrecomp_generated_source_options adds it).
    add_custom_target(xboxrecomp_engine_refresh
        COMMAND "${CMAKE_COMMAND}" --build "${dir}" --target xboxrecomp_engine
        COMMENT "Bringing the prebuilt engine up to date"
        USES_TERMINAL)
    message(STATUS "xboxrecomp: runtime libraries from ${dir} (prebuilt)")
endfunction()
