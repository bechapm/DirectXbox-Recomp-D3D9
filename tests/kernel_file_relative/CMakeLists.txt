# kernel_file_relative -- NtDeleteFile and NtQueryFullAttributesFile with a
# name relative to an open directory (RootDirectory), as XAPI's save code
# uses them (src/kernel/kernel_file.c, translate_obj_path). Portable (Win32
# and POSIX backends). No game files: a temporary game folder and save
# folder, removed afterwards.

add_executable(kernel_file_relative_test test_main.c)
target_include_directories(kernel_file_relative_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/kernel)
target_link_libraries(kernel_file_relative_test PRIVATE xboxrecomp)

add_test(NAME kernel_file_relative COMMAND kernel_file_relative_test)
