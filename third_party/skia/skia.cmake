# Skia - imported pre-built static library (required)
#
# On Windows the Release library is auto-downloaded from the repo's GitHub
# releases when it isn't already present (see BRO_FETCH_SKIA below); headers
# come from the skia submodule. To build it yourself instead (any platform, or
# a Debug lib), populate third_party/skia/lib/{Debug,Release}/:
#   cd third_party/skia/src && python3 tools/git-sync-deps
#   bin/gn gen out/Release --args='...'  (see CLAUDE.md)
#   ninja -C out/Release skia
#   bin/gn gen out/Debug --args='...'
#   ninja -C out/Debug skia

if(WIN32)
    set(_skia_ext "skia.lib")
else()
    set(_skia_ext "libskia.a")
endif()

set(_skia_release "${CMAKE_CURRENT_LIST_DIR}/lib/Release/${_skia_ext}")
set(_skia_debug   "${CMAKE_CURRENT_LIST_DIR}/lib/Debug/${_skia_ext}")

# Auto-fetch the prebuilt Release lib if it isn't already present. Headers still
# come from the skia submodule (see the INTERFACE include below); this only
# grabs the compiled ~37 MB library so contributors don't have to build Skia.
# Only Windows x64 Release is hosted today — other platforms/configs fall
# through to the manual-placement path below, unchanged. BSD-3-Clause permits
# redistributing the binary. Set -DBRO_FETCH_SKIA=OFF to disable.
option(BRO_FETCH_SKIA "Download a prebuilt Skia library when none is present" ON)
set(BRO_SKIA_RELEASE_TAG "skia-prebuilt-v1"
    CACHE STRING "GitHub release (wlejon/bro) holding the prebuilt Skia binaries")

if(BRO_FETCH_SKIA AND WIN32 AND NOT EXISTS "${_skia_release}" AND NOT EXISTS "${_skia_debug}")
    set(_skia_asset "skia-windows-x64-Release.lib")
    set(_skia_sha   "33ab92eb5b8ffac77508945503149cf973d74dddf1049ccdfbf3cbb2d578ba38")
    set(_skia_url   "https://github.com/wlejon/bro/releases/download/${BRO_SKIA_RELEASE_TAG}/${_skia_asset}")
    message(STATUS "Skia: no local library found; downloading prebuilt ${_skia_asset} (~37 MB)...")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/lib/Release")
    file(DOWNLOAD "${_skia_url}" "${_skia_release}.download"
         SHOW_PROGRESS TLS_VERIFY ON STATUS _skia_dl)
    list(GET _skia_dl 0 _skia_dl_code)
    if(_skia_dl_code EQUAL 0)
        file(SHA256 "${_skia_release}.download" _skia_got)
        if(_skia_got STREQUAL _skia_sha)
            file(RENAME "${_skia_release}.download" "${_skia_release}")
            message(STATUS "Skia: prebuilt library ready (Release; used for all configs)")
        else()
            file(REMOVE "${_skia_release}.download")
            message(WARNING "Skia: SHA-256 mismatch on download (got ${_skia_got}); "
                            "ignoring it — place the library manually instead.")
        endif()
    else()
        list(GET _skia_dl 1 _skia_dl_msg)
        file(REMOVE "${_skia_release}.download")
        message(WARNING "Skia: download failed (${_skia_dl_msg}); "
                        "place the library manually per the instructions below.")
    endif()
endif()

# Require at least one configuration
if(NOT EXISTS "${_skia_release}" AND NOT EXISTS "${_skia_debug}")
    message(FATAL_ERROR
        "Skia library not found.\n"
        "Place pre-built Skia in third_party/skia/lib/Release/ and/or third_party/skia/lib/Debug/")
endif()

add_library(skia STATIC IMPORTED GLOBAL)

if(EXISTS "${_skia_release}" AND EXISTS "${_skia_debug}")
    set_target_properties(skia PROPERTIES
        IMPORTED_LOCATION_RELEASE "${_skia_release}"
        IMPORTED_LOCATION_DEBUG   "${_skia_debug}"
        MAP_IMPORTED_CONFIG_MINSIZEREL Release
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
    )
elseif(EXISTS "${_skia_release}")
    set_target_properties(skia PROPERTIES IMPORTED_LOCATION "${_skia_release}")
else()
    set_target_properties(skia PROPERTIES IMPORTED_LOCATION "${_skia_debug}")
endif()

# Skia headers use #include <include/core/...> so the include root is the parent
target_include_directories(skia INTERFACE "${CMAKE_CURRENT_LIST_DIR}/src")

# On macOS, Skia uses CoreText / CoreGraphics for fonts and system image codecs.
if(APPLE)
    target_link_libraries(skia INTERFACE
        "-framework CoreText"
        "-framework CoreGraphics"
        "-framework CoreFoundation"
        "-framework CoreServices"
        "-framework AppKit"
    )
endif()

# On Linux, Skia is built with FreeType + fontconfig — propagate those deps
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FREETYPE2 REQUIRED freetype2)
    pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
    pkg_check_modules(LIBPNG REQUIRED libpng)
    pkg_check_modules(LIBJPEG REQUIRED libjpeg)
    pkg_check_modules(LIBWEBP REQUIRED libwebp libwebpdemux)
    target_link_libraries(skia INTERFACE
        ${FREETYPE2_LIBRARIES}
        ${FONTCONFIG_LIBRARIES}
        ${LIBPNG_LIBRARIES}
        ${LIBJPEG_LIBRARIES}
        ${LIBWEBP_LIBRARIES}
    )
    target_include_directories(skia INTERFACE
        ${FREETYPE2_INCLUDE_DIRS}
        ${FONTCONFIG_INCLUDE_DIRS}
    )
endif()
