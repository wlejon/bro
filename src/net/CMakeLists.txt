add_library(bro_net STATIC
    net_service.cpp
)
target_include_directories(bro_net PUBLIC ${CMAKE_SOURCE_DIR}/src)

# GameNetworkingSockets exports its static and shared consumer targets under
# different names, and any one vcpkg triplet builds only ONE of them:
#   * static triplets (x64-linux, arm64-osx, *-static-md) → GameNetworkingSockets_s
#   * dynamic triplets (x64-windows)                      → GameNetworkingSockets (shared)
# GNS <= 1.4.x happened to build both under the dynamic Windows triplet, so the
# hard-coded _s worked; GNS 1.6.0 builds shared-only there and drops _s. Link
# whichever this build actually provides. The static target propagates
# STEAMNETWORKINGSOCKETS_STATIC_LINK as an INTERFACE define itself (and the
# shared target must NOT have it, for correct dllimport), so we don't set it here.
if(TARGET GameNetworkingSockets::GameNetworkingSockets_s)
    target_link_libraries(bro_net PUBLIC GameNetworkingSockets::GameNetworkingSockets_s bro_util)
else()
    target_link_libraries(bro_net PUBLIC GameNetworkingSockets::GameNetworkingSockets bro_util)
endif()
