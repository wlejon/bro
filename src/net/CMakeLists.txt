# Import the vcpkg GameNetworkingSockets package so the
# GameNetworkingSockets::GameNetworkingSockets_s target below actually exists.
# (Mirrors how src/video/CMakeLists.txt find_package()s its vcpkg deps — without
# this, a clean configure fails with "target not found"; it only appeared to work
# from stale build trees where the target had been imported by an earlier run.)
find_package(GameNetworkingSockets CONFIG REQUIRED)

add_library(bro_net STATIC
    net_service.cpp
)
target_include_directories(bro_net PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_net PUBLIC GameNetworkingSockets::GameNetworkingSockets_s bro_util)
target_compile_definitions(bro_net PUBLIC STEAMNETWORKINGSOCKETS_STATIC_LINK)
