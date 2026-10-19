# input_mouse -- the mouse as the bindings see it: mouse: sources pressing a
# control, and mouse look moving controller 1's right stick on top of what
# is bound to it. No window and no pad: the test feeds the mouse itself, as
# the game window would, and reads controller 1 from the keyboard device.

add_executable(input_mouse_test test_main.c)
target_link_libraries(input_mouse_test PRIVATE xbox_input)
add_test(NAME input_mouse COMMAND input_mouse_test)
