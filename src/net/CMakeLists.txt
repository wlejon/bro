add_library(bro_net STATIC
    net_service.cpp
)
target_include_directories(bro_net PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_net PUBLIC GameNetworkingSockets::GameNetworkingSockets_s bro_util)
target_compile_definitions(bro_net PUBLIC STEAMNETWORKINGSOCKETS_STATIC_LINK)
