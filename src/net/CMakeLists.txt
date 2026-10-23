add_library(bro_net STATIC
    net_service.cpp
    rollback_session.cpp
    bronze_api/net_rollback_api.cpp
)
target_include_directories(bro_net PUBLIC
    ${bro_SOURCE_DIR}/src
    ${bronze_SOURCE_DIR}/src
)
target_compile_definitions(bro_net PRIVATE BRONZE_EMBED_SHARED)

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
    target_link_libraries(bro_net PUBLIC GameNetworkingSockets::GameNetworkingSockets_s bro_util bronze_check)
else()
    target_link_libraries(bro_net PUBLIC GameNetworkingSockets::GameNetworkingSockets bro_util bronze_check)
endif()

if(BRO_BUILD_TESTS)
    add_executable(bro_net_rollback_test
        ${bro_SOURCE_DIR}/tests/test_net_rollback.cpp
    )
    target_include_directories(bro_net_rollback_test PRIVATE
        ${bro_SOURCE_DIR}/src
        ${bro_SOURCE_DIR}/src/bronze_host
        ${bronze_SOURCE_DIR}/src
    )
    target_link_libraries(bro_net_rollback_test PRIVATE
        bro_net
        bronze::runtime_shared
        bronze_check
    )
    target_compile_features(bro_net_rollback_test PRIVATE cxx_std_20)
    add_test(NAME bro_net_rollback_test COMMAND bro_net_rollback_test)
endif()
