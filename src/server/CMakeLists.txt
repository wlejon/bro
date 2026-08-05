add_executable(bro-server main.cpp)
target_link_libraries(bro-server PRIVATE bro_engine brogameagent)
target_include_directories(bro-server PRIVATE ${bro_SOURCE_DIR}/src)

# QuickJS's JS_CallInternal has a large stack frame; deep JS recursion needs
# more than the default 1 MB stack.
if(MSVC)
    target_link_options(bro-server PRIVATE /STACK:8388608)
endif()

if(WIN32)
    target_link_libraries(bro-server PRIVATE ws2_32)
endif()
