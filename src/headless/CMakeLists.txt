add_executable(bro-headless main.cpp)
target_link_libraries(bro-headless PRIVATE bro_engine bro_bronze_host bro_bronze_js)
if(BRO_WITH_GAMEAI)
    target_link_libraries(bro-headless PRIVATE brogameagent)
endif()
bro_bronze_stage_runtime(bro-headless)
target_include_directories(bro-headless PRIVATE ${bro_SOURCE_DIR}/src)

# Set stack size to 8 MB.
if(MSVC)
    target_link_options(bro-headless PRIVATE /STACK:8388608)
endif()

if(WIN32)
    target_link_libraries(bro-headless PRIVATE ws2_32)
endif()
