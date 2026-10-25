# The Windows SDK's library names, for a MinGW build (a self-building
# release's, with zig: tools/release/fetch_toolchain.py).
#
# MSVC's xinput.lib and d3dcompiler.lib are import libraries for
# XInput1_4.dll and D3DCompiler_47.dll; MinGW names them after the DLLs. The
# toolkit and the titles link the SDK's names (a title's CMakeLists says
# `xinput`), so for MinGW each is a target of that name: a target wins over a
# library of the same name, and nothing that links it has to change.
if(WIN32 AND NOT MSVC)
    foreach(_pair "xinput;xinput1_4" "d3dcompiler;d3dcompiler_47")
        list(GET _pair 0 _sdk)
        list(GET _pair 1 _mingw)
        if(NOT TARGET ${_sdk})
            add_library(${_sdk} INTERFACE IMPORTED GLOBAL)
            set_property(TARGET ${_sdk} PROPERTY INTERFACE_LINK_LIBRARIES ${_mingw})
        endif()
    endforeach()
endif()
