# Title updates and the launch defaults (xbox_title_update.c), with the host
# and the settings stubbed. Portable.
add_executable(test_title_update
    test_title_update.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel/xbox_title_update.c
)
target_include_directories(test_title_update PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/config
)
add_test(NAME title_update COMMAND test_title_update)
