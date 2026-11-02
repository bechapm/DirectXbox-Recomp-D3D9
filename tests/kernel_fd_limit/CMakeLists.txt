# kernel_fd_limit -- the POSIX open-file limit: raised at kernel init
# (w32_raise_fd_limit in src/platform/win32_compat.c), and what the file HLE
# answers when it runs out anyway. POSIX only. No game files: a temporary
# game folder and save folder, removed afterwards.

add_executable(kernel_fd_limit_test test_main.c)
target_include_directories(kernel_fd_limit_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/kernel)
target_link_libraries(kernel_fd_limit_test PRIVATE xboxrecomp)

add_test(NAME kernel_fd_limit COMMAND kernel_fd_limit_test)
