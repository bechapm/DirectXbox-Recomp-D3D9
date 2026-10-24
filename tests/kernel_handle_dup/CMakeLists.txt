# kernel_handle_dup -- NtDuplicateObject gives a duplicate its own handle
# token, and closing the source leaves it working, on either file backend.
# Includes src/kernel/kernel_bridge.c to reach its private handle table and
# bridges. No game files.

add_executable(kernel_handle_dup_test test_main.c)
target_include_directories(kernel_handle_dup_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/kernel)
target_link_libraries(kernel_handle_dup_test PRIVATE xboxrecomp)

add_test(NAME kernel_handle_dup COMMAND kernel_handle_dup_test)
