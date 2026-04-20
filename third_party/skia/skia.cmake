# Skia - imported pre-built static library (required)
#
# Populate third_party/skia/lib/{Debug,Release}/ with Skia builds.
# Build instructions:
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
