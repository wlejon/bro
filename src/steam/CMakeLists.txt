# bro.steam — Steamworks social/voice/UGC binding (handoff §3.2).
#
# Gated: builds an inert, always-present stub by default (bro.steam.available
# == false). The real binding compiles only with BRO_WITH_STEAM=ON and a
# Steamworks SDK supplied OUT OF BAND — the SDK is proprietary and is never
# committed (see .gitignore: third_party/steamworks/).
#
# The stub path (default) has no SDK dependency at all, so a normal bro build
# is unaffected.

option(BRO_WITH_STEAM "Enable Steamworks (friends/lobbies/voice/UGC)" OFF)
set(STEAMWORKS_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/steamworks"
    CACHE PATH "External Steamworks SDK root (proprietary; supplied out of band)")

add_library(bro_steam STATIC steam_service.cpp)
target_include_directories(bro_steam PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_steam PUBLIC bro_util)

if(BRO_WITH_STEAM)
    if(NOT EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h")
        message(FATAL_ERROR
            "BRO_WITH_STEAM=ON but no Steamworks SDK found at '${STEAMWORKS_SDK_DIR}' "
            "(expected '${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h'). "
            "Download the SDK from partner.steamgames.com and point at it with "
            "-DSTEAMWORKS_SDK_DIR=/path/to/sdk.")
    endif()

    target_compile_definitions(bro_steam PUBLIC BRO_WITH_STEAM)
    # Steamworks headers are visible to THIS target only (PRIVATE), so its
    # superset `steam/` tree can't collide with the GameNetworkingSockets
    # `steam/` tree the rest of the build uses (handoff §3.1). steam_service.h
    # exposes no SDK types, so nothing leaks to consumers.
    target_include_directories(bro_steam PRIVATE "${STEAMWORKS_SDK_DIR}/public")

    if(WIN32)
        target_link_libraries(bro_steam PUBLIC
            "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.lib")
    elseif(APPLE)
        target_link_libraries(bro_steam PUBLIC
            "${STEAMWORKS_SDK_DIR}/redistributable_bin/osx/libsteam_api.dylib")
    else()
        target_link_libraries(bro_steam PUBLIC
            "${STEAMWORKS_SDK_DIR}/redistributable_bin/linux64/libsteam_api.so")
    endif()

    # NOTE (ON-path integration, deferred): copy the redistributable
    # (steam_api64.dll / libsteam_api.{so,dylib}) and a dev steam_appid.txt (480)
    # next to the built bro/bro-headless executables as a post-build step. Those
    # targets are defined in src/CMakeLists.txt (after this subdirectory), so the
    # copy is wired there, guarded by BRO_WITH_STEAM, when the SDK lands.
    message(STATUS "bro.steam: Steamworks ENABLED (SDK: ${STEAMWORKS_SDK_DIR})")
else()
    message(STATUS "bro.steam: stub build (BRO_WITH_STEAM=OFF) — bro.steam.available will be false")
endif()
