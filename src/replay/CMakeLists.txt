# d3d8_replay -- replay a captured frame into the host renderer.
#
# Every platform: on Windows into a window, through D3D11 or Vulkan
# (--backend); elsewhere through Vulkan with no window at all (a headless
# swap chain, rhi.h), which is what scripts/replay_ab.py and the tests want.
#
# It compiles d3d8_capture.c directly rather than linking xbox_hle, because
# the container has no dependencies and xbox_hle would drag in the kernel
# layer, XAudio2 and XInput for a tool that needs none of them. The mods'
# loader (recomp_mods.c) likewise: it is handed the title rather than
# reaching for it, so a replay loads the same mods a title does.

# nv2a_vsh.c is built in rather than linked: xbox_d3d8's vertex program
# translator calls the microcode decoder, which lives in src/kernel and is
# otherwise only reachable through xbox_kernel. tests/nv2a_vsh_hlsl compiles
# it the same way, for the same reason -- neither wants the kernel layer.
add_executable(d3d8_replay
    d3d8_replay.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../hle/d3d8_capture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../hle/recomp_mods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../kernel/nv2a_vsh.c
)
target_include_directories(d3d8_replay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../hle
    ${CMAKE_CURRENT_SOURCE_DIR}/../kernel
)
target_link_libraries(d3d8_replay PRIVATE xbox_d3d8)
if(WIN32)
    target_link_libraries(d3d8_replay PRIVATE user32)
elseif(TARGET xbox_host)
    # --window (macOS): a real window from src/host, in background mode.
    target_link_libraries(d3d8_replay PRIVATE xbox_host)
    target_include_directories(d3d8_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../host)
    target_compile_definitions(d3d8_replay PRIVATE REPLAY_HAVE_HOST)
endif()
