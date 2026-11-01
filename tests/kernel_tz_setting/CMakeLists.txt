# kernel_tz_setting -- the EEPROM time zone is the host's, with the sign NT
# uses (UTC = local + bias), and the XC_MAX_OS block agrees with the single
# queries (src/kernel/kernel_xbox.c). Portable. No files.

add_executable(kernel_tz_setting_test test_main.c)
target_include_directories(kernel_tz_setting_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/kernel)
target_link_libraries(kernel_tz_setting_test PRIVATE xboxrecomp)

add_test(NAME kernel_tz_setting COMMAND kernel_tz_setting_test)
