add_executable(bro-server main.cpp)
target_link_libraries(bro-server PRIVATE bro_engine)
if(BRO_WITH_GAMEAI)
    target_link_libraries(bro-server PRIVATE brogameagent)
endif()
if(BRO_WITH_BRONZE)
    target_link_libraries(bro-server PRIVATE bro_bronze_host)
    bro_bronze_stage_runtime(bro-server)
endif()
target_include_directories(bro-server PRIVATE ${bro_SOURCE_DIR}/src)

# Set stack size to 8 MB.
if(MSVC)
    target_link_options(bro-server PRIVATE /STACK:8388608)
endif()

if(WIN32)
    target_link_libraries(bro-server PRIVATE ws2_32)
endif()
