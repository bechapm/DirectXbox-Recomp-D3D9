# The toolkit's icon (assets/icon) on the executables it builds.
#
# SDL windows (the launcher and the game, off Windows) set it at run time from
# src/config/recomp_icon.h. On Windows an executable's icon is a resource
# compiled into it: the launcher gets one in recomp_add_launcher, and the
# title's own executable gets one here, added once the title's CMakeLists is
# done, so no title has to change. Its windows then load it by id 1
# (launcher_win32.c, src/hle/hle_d3d8.c).

# The resource script naming the .ico, written once into the build folder:
# an absolute path, so rc.exe finds it from wherever it runs.
function(xboxrecomp_icon_rc OUT)
    get_filename_component(_ico "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../assets/icon/xboxrecomp.ico"
                           ABSOLUTE)
    set(_rc "${CMAKE_BINARY_DIR}/xboxrecomp_icon.rc")
    set(_text "1 ICON \"${_ico}\"\n")
    if(EXISTS "${_rc}")
        file(READ "${_rc}" _old)
    endif()
    if(NOT _old STREQUAL _text)
        file(WRITE "${_rc}" "${_text}")
    endif()
    set(${OUT} "${_rc}" PARENT_SCOPE)
endfunction()

function(xboxrecomp_title_icon)
    if(WIN32 AND TARGET ${CMAKE_PROJECT_NAME})
        get_target_property(_type ${CMAKE_PROJECT_NAME} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            xboxrecomp_icon_rc(_rc)
            target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${_rc}")
        endif()
    endif()
endfunction()

if(WIN32 AND NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL xboxrecomp_title_icon)
endif()
