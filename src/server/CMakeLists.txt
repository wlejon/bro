add_executable(bro-server main.cpp)
# BRO_BRASS_LIB first: see the note above bro_bronze_host's link line.
target_link_libraries(bro-server PRIVATE ${BRO_BRASS_LIB} bro_engine bro_bronze_host)
if(BRO_WITH_GAMEAI)
    target_link_libraries(bro-server PRIVATE brogameagent)
endif()
bro_bronze_stage_runtime(bro-server)
target_include_directories(bro-server PRIVATE ${bro_SOURCE_DIR}/src)

# Set stack size to 8 MB.
if(MSVC)
    target_link_options(bro-server PRIVATE /STACK:8388608)
endif()

if(WIN32)
    target_link_libraries(bro-server PRIVATE ws2_32)
endif()
