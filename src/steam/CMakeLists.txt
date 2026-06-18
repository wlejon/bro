# bro.steam — Steamworks social/voice/UGC binding (handoff §3).
#
# bro depends on NO Steamworks SDK at build time. The service talks to the Steam
# redistributable (steam_api64.dll / libsteam_api.{so,dylib}) through the stable
# flat C API, resolved at runtime (see steam_flat.{h,cpp}). So there are no
# proprietary headers or import libs here — the binding is always built and real,
# and probes at runtime like bro.gpu. The redistributable is never committed (it
# ships with the product; gitignored) and a normal bro build is unaffected.

add_library(bro_steam STATIC
    steam_service.cpp
    steam_flat.cpp
)
target_include_directories(bro_steam PUBLIC ${CMAKE_SOURCE_DIR}/src)
# ${CMAKE_DL_LIBS} = libdl on Linux for dlopen/dlsym; empty on Windows/macOS.
target_link_libraries(bro_steam PUBLIC bro_util ${CMAKE_DL_LIBS})
