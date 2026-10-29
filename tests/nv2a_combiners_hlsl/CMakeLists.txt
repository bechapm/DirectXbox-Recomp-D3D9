# nv2a_combiners_hlsl -- compile the register combiner translator's output
# for 400 pseudo-random combiner configurations, each without and with the
# alpha test (800 shaders): D3DCompile on Windows, DXC to
# SPIR-V elsewhere (rhi_vulkan_dxc.cpp, built in). Links the same sources as
# tests/d3d8_smoke's d3d8_final_default, with the same device stubs. No
# device, window or game files; skipped (77) without a compiler.

set(XBR_COMBINERS_TEST_SOURCES
    test_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_resources.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_shaders.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_combiners.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi_d3d9.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi_shader_cache.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_compile.c
)

if(WIN32)
    add_executable(nv2a_combiners_hlsl_test ${XBR_COMBINERS_TEST_SOURCES}
                   ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi_d3d11.c)
    target_link_libraries(nv2a_combiners_hlsl_test PRIVATE
        platform d3d11 dxgi dxguid d3dcompiler xbox_config)
else()
    add_executable(nv2a_combiners_hlsl_test ${XBR_COMBINERS_TEST_SOURCES}
                   ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/rhi_vulkan_dxc.cpp)
    target_include_directories(nv2a_combiners_hlsl_test PRIVATE ${XBOXRECOMP_DXC_INCLUDE})
    if(XBOXRECOMP_DXC_LIBRARY)
        target_compile_definitions(nv2a_combiners_hlsl_test PRIVATE
            "RHI_DXC_LIBRARY=\"${XBOXRECOMP_DXC_LIBRARY}\"")
    endif()
    target_link_libraries(nv2a_combiners_hlsl_test PRIVATE platform xbox_config ${CMAKE_DL_LIBS})
endif()
target_include_directories(nv2a_combiners_hlsl_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/config)

add_test(NAME nv2a_combiners_hlsl COMMAND nv2a_combiners_hlsl_test)
set_tests_properties(nv2a_combiners_hlsl PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                     ENVIRONMENT "RECOMP_SHADER_CACHE=0")
