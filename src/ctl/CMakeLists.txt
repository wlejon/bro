# bro-ctl: the shell client of the agent control channel (docs/agent-control.md).
# Standalone — no engine, nothing of bro's but the header-only protocol — so
# it builds in any tree that builds bro, including a host that embeds
# bro_engine (helm) and turns the stock executables off. Its transport is
# brolink's local IPC: a Unix socket on POSIX, a named pipe on Windows.
add_executable(bro-ctl bro_ctl.cpp)
set_target_properties(bro-ctl PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_include_directories(bro-ctl PRIVATE ${bro_SOURCE_DIR}/src)
target_link_libraries(bro-ctl PRIVATE brolink)
