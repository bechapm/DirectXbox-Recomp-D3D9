# A prebuilt engine package: what a title's add_subdirectory(${XBOXRECOMP_DIR})
# finds in a self-building release, in place of the toolkit's own CMakeLists.
#
# tools/release/make_engine_package.py copies this file to the root of the
# package, beside the runtime libraries, the headers they were built with and
# xbox_config's source. It does for a title what the toolkit does, without
# building the runtime: imports the libraries (cmake/prebuilt_engine.cmake),
# builds xbox_config with the title's own settings, and gives lifted code its
# flags (cmake/lifted_code.cmake). Nothing in the title's CMakeLists changes.

cmake_minimum_required(VERSION 3.20)

get_property(_xboxrecomp_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _xboxrecomp_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type for single-config generators (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()
unset(_xboxrecomp_multi_config)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    add_compile_options(/we4013)
else()
    add_compile_options(-Werror=implicit-function-declaration)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/prebuilt_engine.cmake)
xboxrecomp_import_engine("${CMAKE_CURRENT_LIST_DIR}")

# The title's own widescreen and timing settings go into this one.
add_subdirectory(src/config)

# The release ships its launcher, built before any game exists; a title's
# call to make one is answered here with nothing.
function(recomp_add_launcher TARGET GAME_EXE)
endfunction()

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

include(${CMAKE_CURRENT_LIST_DIR}/cmake/lifted_code.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/title_icon.cmake)
