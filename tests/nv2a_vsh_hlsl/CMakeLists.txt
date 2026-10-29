# nv2a_vsh_hlsl -- compile the HLSL the vertex-program translator generates.
#
# On Windows through D3DCompile; elsewhere through DXC to SPIR-V, the way the
# Vulkan backend compiles (rhi_vulkan_dxc.cpp, built in). Builds d3d8_vsh.c
# and nv2a_vsh.c with stubs for the device accessors d3d8_vsh.c links
# against, the same way tests/d3d8_smoke builds its targets. No device,
# window or game files.

if(NOT WIN32)
    add_executable(nv2a_vsh_hlsl_test
        test_main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_vsh.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel/nv2a_vsh.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi_d3d9.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi_shader_cache.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_compile.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi_vulkan_dxc.cpp
    )
    target_include_directories(nv2a_vsh_hlsl_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
        ${XBOXRECOMP_DXC_INCLUDE}
    )
    if(XBOXRECOMP_DXC_LIBRARY)
        target_compile_definitions(nv2a_vsh_hlsl_test PRIVATE
            "RHI_DXC_LIBRARY=\"${XBOXRECOMP_DXC_LIBRARY}\"")
    endif()
    target_link_libraries(nv2a_vsh_hlsl_test PRIVATE platform xbox_config ${CMAKE_DL_LIBS})
    add_test(NAME nv2a_vsh_hlsl COMMAND nv2a_vsh_hlsl_test)
    # 77: no DXC on this machine, which is a skip rather than a failure.
    set_tests_properties(nv2a_vsh_hlsl PROPERTIES SKIP_RETURN_CODE 77
                         ENVIRONMENT "RECOMP_SHADER_CACHE=0")
    return()
endif()

add_executable(nv2a_vsh_hlsl_test
    test_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_vsh.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel/nv2a_vsh.c
    # d3d8_vsh.c creates its objects through rhi.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi_d3d9.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi_d3d11.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi_shader_cache.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_compile.c
)
target_include_directories(nv2a_vsh_hlsl_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
)
# xbox_config: d3d8_vsh.c reads its settings through recomp_config.h, and
# without it this test has not compiled since that include arrived.
target_link_libraries(nv2a_vsh_hlsl_test PRIVATE
    platform d3d11 dxgi dxguid d3dcompiler xbox_config
)

add_test(NAME nv2a_vsh_hlsl COMMAND nv2a_vsh_hlsl_test)
