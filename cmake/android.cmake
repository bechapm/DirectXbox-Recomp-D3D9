# Android: the engine and the title as two libraries.
#
# An Android APK carries no game code: the title is compiled on the player's
# device from their own disc (src/host/android_build.c), into libgame.so, and
# the APK's libmain.so is the engine alone. libmain.so is what SDL's Java
# activity loads; it loads libgame.so before the title runs and binds the
# engine's few references to it (src/host/android_title.c).
#
# A title's CMakeLists, on Android, makes its sources an OBJECT library and
# calls this with it:
#
#   add_library(${PROJECT_NAME} OBJECT ${SOURCES})
#   xboxrecomp_android_libraries(${PROJECT_NAME})
#
# which adds two targets:
#
#   main   libmain.so: every engine library linked whole, since the title's
#          calls into it are not known when it is built, and the Android
#          entry (xbox_android_main)
#   game   libgame.so from the title's objects, linked against libmain.so:
#          what the device builds with zig, made here the same way to test
#          it on a developer's machine
#
# The engine is compiled with XBOXRECOMP_SPLIT_TITLE (CMakeLists.txt) when
# XBOXRECOMP_ANDROID_SPLIT is on, the default.

# pyexec: Python as a program, for the on-device build (android/pyexec.c).
# Named lib*.so so that the APK carries it in its library folder, the one
# place Android starts programs from.
if(ANDROID AND NOT TARGET pyexec)
    add_executable(pyexec ${CMAKE_CURRENT_LIST_DIR}/../android/pyexec.c)
    set_target_properties(pyexec PROPERTIES OUTPUT_NAME libpyexec SUFFIX ".so")
    target_link_libraries(pyexec PRIVATE ${CMAKE_DL_LIBS})
endif()

function(xboxrecomp_android_libraries title_objects)
    set(engine xbox_kernel xbox_d3d8 xbox_dsound xbox_input xbox_usb xbox_apu
               xbox_nv2a xbox_video xbox_hle xbox_host platform xbox_config)
    if(TARGET xbox_net)
        list(APPEND engine xbox_net)
    endif()
    string(REPLACE ";" "," engine_list "${engine}")

    add_library(main SHARED $<TARGET_OBJECTS:xbox_android_main>)
    target_link_libraries(main PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,${engine_list}>" SDL3::SDL3 android log)
    # Nothing the title will need may be left out unseen.
    target_link_options(main PRIVATE -Wl,--no-undefined)

    # Lifted code calls lifted code tens of thousands of times a frame. In a
    # shared library those calls would go through the dynamic linker's tables
    # in case another library replaced them, which nothing does: the title's
    # own calls bind to itself.
    target_compile_options(${title_objects} PRIVATE -fno-semantic-interposition)
    add_library(game SHARED $<TARGET_OBJECTS:${title_objects}>)
    target_link_libraries(game PRIVATE main)
    target_link_options(game PRIVATE -Wl,-Bsymbolic)
    set_target_properties(game PROPERTIES OUTPUT_NAME game)
endfunction()
