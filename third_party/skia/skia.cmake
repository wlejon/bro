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
    )
elseif(EXISTS "${_skia_release}")
    set_target_properties(skia PROPERTIES IMPORTED_LOCATION "${_skia_release}")
else()
    set_target_properties(skia PROPERTIES IMPORTED_LOCATION "${_skia_debug}")
endif()

# Skia headers use #include <include/core/...> so the include root is the parent
target_include_directories(skia INTERFACE "${CMAKE_CURRENT_LIST_DIR}/src")
