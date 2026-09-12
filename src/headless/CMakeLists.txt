add_executable(bro-headless main.cpp)
# BRO_BRASS_LIB first: see the note above bro_bronze_host's link line.
target_link_libraries(bro-headless PRIVATE ${BRO_BRASS_LIB} bro_engine bro_bronze_host)
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
