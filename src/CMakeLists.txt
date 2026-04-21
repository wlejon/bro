add_subdirectory(util)
add_subdirectory(platform)
add_subdirectory(render)
add_subdirectory(svg)
add_subdirectory(js)
add_subdirectory(dom)
add_subdirectory(layout)
add_subdirectory(canvas)
add_subdirectory(webgl)
add_subdirectory(physics)
add_subdirectory(scene)
add_subdirectory(net)
add_subdirectory(engine)
add_subdirectory(headless)
add_subdirectory(server)

# WIN32 = /SUBSYSTEM:WINDOWS — no console window on double-click.
# main.cpp calls AttachConsole() at startup so stdout still shows when
# launched from a terminal. Ignored on non-Windows platforms.
add_executable(bro WIN32 main.cpp)
target_link_libraries(bro PRIVATE bro_engine)
target_include_directories(bro PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# QuickJS's JS_CallInternal has a large stack frame; deep JS recursion
# (e.g. Vue template compiler) needs more than the default 1 MB stack.
if(MSVC)
    target_link_options(bro PRIVATE /STACK:8388608)
    # GUI subsystem but keep int main() as the entry point.
    target_link_options(bro PRIVATE /ENTRY:mainCRTStartup)
endif()

if(WIN32)
    target_link_libraries(bro PRIVATE ws2_32)
endif()
