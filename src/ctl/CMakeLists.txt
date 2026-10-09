# bro-ctl: the shell client of the agent control socket (docs/agent-control.md).
# Standalone — no engine, no libraries — so it builds in any tree that builds
# bro, including a host that embeds bro_engine (helm) and turns the stock
# executables off. An AF_UNIX socket everywhere: POSIX, and Winsock on Windows.
add_executable(bro-ctl bro_ctl.cpp)
set_target_properties(bro-ctl PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
    target_link_libraries(bro-ctl PRIVATE ws2_32)
endif()
