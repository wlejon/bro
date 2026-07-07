# Skia - imported pre-built static library (required)
#
# On Windows, Linux, and Apple Silicon (arm64) macOS, both the Skia headers/source
# and the Release library are auto-downloaded from the repo's GitHub releases when
# absent (see BRO_FETCH_SKIA below) — all pinned to one Skia commit so they always
# match. To build it yourself instead (Intel macOS, a Windows Debug lib, or a
# different Skia version),
# run third_party/skia/build_skia_{linux,mac}.sh, or populate
# third_party/skia/lib/{Debug,Release}/ and third_party/skia/src/ by hand:
#   cd third_party/skia/src && python3 tools/git-sync-deps
#   bin/gn gen out/Release --args='...'  (see CLAUDE.md)
#   ninja -C out/Release skia

if(WIN32)
    set(_skia_ext "skia.lib")
else()
    set(_skia_ext "libskia.a")
endif()

set(_skia_release "${CMAKE_CURRENT_LIST_DIR}/lib/Release/${_skia_ext}")
set(_skia_debug   "${CMAKE_CURRENT_LIST_DIR}/lib/Debug/${_skia_ext}")

# Auto-fetch the prebuilt Skia headers/source and the Release library when they
# aren't already present. Everything is pinned to one Skia commit and served
# from a single GitHub release, so a fetched lib always matches fetched headers.
# The source bundle is the subset bro compiles/includes (include/, src/, the
# svg/skshaper/skresources modules, and expat), not the whole Skia tree.
# Non-fatal: a failed download / hash mismatch falls through to the manual
# instructions below. Windows Debug and Intel (x86_64) macOS are not hosted.
# BSD-3-Clause permits redistribution. Set -DBRO_FETCH_SKIA=OFF to disable.
option(BRO_FETCH_SKIA "Download prebuilt Skia (headers + lib) when absent" ON)
set(BRO_SKIA_RELEASE_TAG "skia-prebuilt-m147"
    CACHE STRING "GitHub release (wlejon/bro) holding the prebuilt Skia binaries")
set(_skia_base "https://github.com/wlejon/bro/releases/download/${BRO_SKIA_RELEASE_TAG}")

# Download _url -> _dest, verifying SHA-256 _sha. Sets ${_okvar} in the caller.
function(_bro_skia_download _url _dest _sha _okvar)
    file(DOWNLOAD "${_url}" "${_dest}.part" SHOW_PROGRESS TLS_VERIFY ON STATUS _st)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _st 1 _msg)
        file(REMOVE "${_dest}.part")
        message(WARNING "Skia: download failed for ${_url} (${_msg})")
        set(${_okvar} FALSE PARENT_SCOPE)
        return()
    endif()
    file(SHA256 "${_dest}.part" _got)
    if(NOT _got STREQUAL _sha)
        file(REMOVE "${_dest}.part")
        message(WARNING "Skia: SHA-256 mismatch for ${_url} (got ${_got})")
        set(${_okvar} FALSE PARENT_SCOPE)
        return()
    endif()
    file(RENAME "${_dest}.part" "${_dest}")
    set(${_okvar} TRUE PARENT_SCOPE)
endfunction()

if(BRO_FETCH_SKIA)
    # (1) Headers + source bundle (platform-independent).
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/include/core/SkCanvas.h")
        set(_skia_bundle "${CMAKE_CURRENT_LIST_DIR}/skia-src.tar.gz")
        message(STATUS "Skia: fetching source bundle (headers + svg/expat sources, ~6 MB)...")
        _bro_skia_download("${_skia_base}/skia-src-m147.tar.gz" "${_skia_bundle}"
            "ce6125dc0818ec3a07c48331cdeb827556ce8a608521cbd384846b00069cab8e" _skia_src_ok)
        if(_skia_src_ok)
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/src")
            file(ARCHIVE_EXTRACT INPUT "${_skia_bundle}"
                 DESTINATION "${CMAKE_CURRENT_LIST_DIR}/src")
            file(REMOVE "${_skia_bundle}")
            message(STATUS "Skia: source bundle extracted to src/")
        endif()
    endif()

    # (2) Platform Release library.
    if(NOT EXISTS "${_skia_release}" AND NOT EXISTS "${_skia_debug}")
        set(_skia_lib_asset "")
        if(WIN32)
            set(_skia_lib_asset "skia-windows-x64-Release.lib")
            set(_skia_lib_sha "e4e561366ac923218406c9f9a027ed23401b44f7c40b041a9fab5fddb4e46823")
        elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
            # Only Apple Silicon (arm64) is hosted; Intel Macs fall through to
            # build_skia_mac.sh rather than fetch an unlinkable arm64 lib.
            set(_skia_lib_asset "skia-macos-arm64-Release.a")
            set(_skia_lib_sha "42fc7231974cc0011f09e343cadb31935be5019669125ee261fea7fbe947481d")
        elseif(UNIX AND NOT APPLE)
            set(_skia_lib_asset "skia-linux-x64-Release.a")
            set(_skia_lib_sha "adb014b9eb366266d205b293258d9a9628f73b9ea7156da2053e65e3f3363f55")
        endif()
        if(_skia_lib_asset)
            message(STATUS "Skia: fetching prebuilt ${_skia_lib_asset}...")
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/lib/Release")
            _bro_skia_download("${_skia_base}/${_skia_lib_asset}" "${_skia_release}"
                "${_skia_lib_sha}" _skia_lib_ok)
            if(_skia_lib_ok)
                message(STATUS "Skia: prebuilt library ready (Release; used for all configs)")
            endif()
        endif()
    endif()
endif()

# Headers must be present (fetched above or supplied by hand) before we can build.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/include/core/SkCanvas.h")
    message(FATAL_ERROR
        "Skia headers not found at third_party/skia/src/.\n"
        "Enable BRO_FETCH_SKIA (default), or run third_party/skia/build_skia_*.sh, "
        "or clone Skia into third_party/skia/src and run tools/git-sync-deps.")
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
